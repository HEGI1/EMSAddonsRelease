//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAutosaveSubsystem.h"

#include "EMSAddonsAuthority.h"
#include "EMSAddonsAutosave.h"
#include "EMSAsyncLoadGame.h"
#include "EMSAsyncSaveGame.h"
#include "EMSAutosaveSettings.h"
#include "EMSCheckpoint.h"
#include "EMSCheckpointSaveGame.h"
#include "EMSCheckpointUtils.h"
#include "EMSMisc.h"
#include "EMSObject.h"
#include "EMSPaths.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	const FName DefaultAutosaveReason(TEXT("Autosave"));
	const FName CheckpointAutosaveReason(TEXT("Checkpoint"));
	const FName PeriodicAutosaveReason(TEXT("Periodic"));
	const FName MapLoadAutosaveReason(TEXT("MapLoad"));
	const FName ManualSaveReason(TEXT("Manual"));
	constexpr float MapLoadAutosaveRetryDelay = 0.1f;

	/** An interrupted save is recovered once per process, by the first game instance. */
	bool bInterruptedSaveChecked = false;
}

void UEMSAutosaveSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UEMSObject>();
	bIsShuttingDown = false;
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UEMSAutosaveSubsystem::HandlePostLoadMap);
	FWorldDelegates::OnWorldCleanup.AddUObject(this, &UEMSAutosaveSubsystem::HandleWorldCleanup);
	if (GEngine)
	{
		GEngine->OnTravelFailure().AddUObject(this, &UEMSAutosaveSubsystem::HandleTravelFailure);
	}

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UEMSObject* EMS = GameInstance->GetSubsystem<UEMSObject>())
		{
			EMS->OnPlayerLoaded.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleEMSPlayerLoaded);
			EMS->OnLevelLoaded.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleEMSLevelLoaded);
			EMS->OnActorsSaved.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleEMSActorsSaved);
		}
	}

	if (!bInterruptedSaveChecked)
	{
		bInterruptedSaveChecked = true;
		RecoverInterruptedSave();
	}

	if (UWorld* World = GetWorld())
	{
		ConfigureWorldTimers(World);
	}
}

void UEMSAutosaveSubsystem::Deinitialize()
{
	bIsShuttingDown = true;
	FCoreUObjectDelegates::PostLoadMapWithWorld.RemoveAll(this);
	FWorldDelegates::OnWorldCleanup.RemoveAll(this);
	if (GEngine)
	{
		GEngine->OnTravelFailure().RemoveAll(this);
	}

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UEMSObject* EMS = GameInstance->GetSubsystem<UEMSObject>())
		{
			EMS->OnPlayerLoaded.RemoveDynamic(this, &UEMSAutosaveSubsystem::HandleEMSPlayerLoaded);
			EMS->OnLevelLoaded.RemoveDynamic(this, &UEMSAutosaveSubsystem::HandleEMSLevelLoaded);
			EMS->OnActorsSaved.RemoveDynamic(this, &UEMSAutosaveSubsystem::HandleEMSActorsSaved);
		}
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
	}
	if (PendingCheckpointActor.IsValid())
	{
		PendingCheckpointActor->MarkActivationFailed();
	}
	if (ActiveCheckpointActor.IsValid())
	{
		ActiveCheckpointActor->MarkActivationFailed();
	}
	// Shutting down mid-save: EMS remembers the current slot for the next start,
	// so point it back at the last complete save. While the EMS task still runs EMS
	// refuses the switch; the journal then lets the next start recover.
	const bool bReturned = !bSaveActive || ReturnToPreviousSlot(ActivePreviousSlot, ActiveSaveSlot, EEMSSlotSwitch::LeaveWorld);
	AutosaveBlockers.Empty();
	PendingRequest.Reset();
	PendingCheckpointActor.Reset();
	ResetActiveSaveState(bReturned);
	ActiveLoadTask = nullptr;
	CheckpointBeingLoaded = FEMSCheckpointRecord();
	CheckpointAwaitingPlayerPlacement = FEMSCheckpointRecord();
	CheckpointLoadSlot.Reset();
	SlotBeforeTravelLoad.Reset();
	AddonSaveSlotAwaitingActorsSaved.Reset();
	bCheckpointLoadAfterTravel = false;
	bIgnoreNextSuccessfulActorsSaved = false;
	ResetSaveLoadState();
	Super::Deinitialize();
}

bool UEMSAutosaveSubsystem::RequestAutosave(FName Reason, FText DisplayName)
{
	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	if (!Settings
		|| !Settings->bEnableAutosave
		|| bIsShuttingDown
		|| IsCheckpointLoadInProgress()
		|| IsSaveLoadInProgress()
		|| FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_All, false))
	{
		return false;
	}

	UWorld* World = GetWorld();
	UEMSObject* EMS = UEMSObject::Get(this);
	if (!World
		|| !World->IsGameWorld()
		|| World->bIsTearingDown
		|| !EMSAddons::HasPersistenceAuthority(World->GetNetMode())
		|| !EMS)
	{
		return false;
	}

	// Requests belong to the profile, not to one slot: every save moves the current slot.
	const FString Profile = GetSaveProfile();
	return !Profile.IsEmpty()
		&& QueueRequest(Reason.IsNone() ? DefaultAutosaveReason : Reason, Profile, nullptr, nullptr,
			DisplayName.IsEmptyOrWhitespace() ? AutosaveDisplayName : DisplayName);
}

bool UEMSAutosaveSubsystem::ActivateCheckpoint(AEMSCheckpoint* Checkpoint)
{
	if (bIsShuttingDown
		|| IsCheckpointLoadInProgress()
		|| IsSaveLoadInProgress()
		|| FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_All, false)
		|| !IsValid(Checkpoint)
		|| Checkpoint->IsActivationPending()
		|| !EMSAddons::HasPersistenceAuthority(Checkpoint)
		|| Checkpoint->GetWorld() != GetWorld()
		|| !Checkpoint->CheckpointId.IsValid())
	{
		return false;
	}
	if (!Checkpoint->HasUniqueCheckpointId())
	{
		LogMessage(FString::Printf(TEXT("Checkpoint activation rejected because ID %s is duplicated. Actor=%s"), *Checkpoint->CheckpointId.ToString(), *Checkpoint->GetPathName()), true);
		return false;
	}

	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS)
	{
		return false;
	}
	const FString Profile = GetSaveProfile();
	if (Profile.IsEmpty())
	{
		return false;
	}

	FEMSCheckpointRecord Record;
	Record.CheckpointId = Checkpoint->CheckpointId;
	Record.DisplayName = Checkpoint->DisplayName;
	Record.World = EMSCheckpoint::GetWorldAssetPath(Checkpoint->GetWorld());
	Record.ActivatedAt = FDateTime::UtcNow();
	Record.SaveSlot = ResolveSaveSlotTarget(MakeCheckpointSlotName(Profile, Checkpoint->CheckpointSlotName));
	if (!Record.IsValid())
	{
		return false;
	}

	// Close re-entrancy before Blueprint callbacks can activate this actor again.
	Checkpoint->MarkActivationPending();

	// Re-entering the current checkpoint does not save again, as long as its save still exists.
	const FEMSCheckpointRecord Existing = GetCurrentCheckpoint();
	if (Existing.IsValid()
		&& Existing.HasSameIdentity(Record)
		&& EMS->DoesSaveSlotExist(Existing.SaveSlot)
		&& !Checkpoint->bForceSaveNextActivation)
	{
		if (Checkpoint->bTriggerOnce)
		{
			// The actor may have been reconstructed by a respawn or a streamed-level
			// lifecycle after the checkpoint save. The committed slot metadata is the
			// durable source of truth in that case; do not let the reconstructed actor
			// broadcast a second one-shot activation.
			Checkpoint->MarkActivationCommitted();
			return false;
		}

		Checkpoint->MarkActivationCommitted();
		OnCheckpointActivated.Broadcast(Record);
		return true;
	}

	// A checkpoint is a saved restore point by definition. The general autosave
	// enable switch only controls Request Autosave and the automatic helper timers;
	// it never turns a checkpoint into an event-only trigger.
	// Queue without starting so accepted activation is observable before the EMS
	// save begins, matching the public delegate contract.
	if (!EnqueueRequest(CheckpointAutosaveReason, Profile, &Record, Checkpoint))
	{
		Checkpoint->MarkActivationFailed();
		return false;
	}
	OnCheckpointActivated.Broadcast(Record);
	TryStartPendingSave();
	return true;
}

bool UEMSAutosaveSubsystem::QueueRequest(
	FName Reason,
	const FString& Profile,
	const FEMSCheckpointRecord* CheckpointRecord,
	AEMSCheckpoint* CheckpointActor,
	const FText& DisplayName)
{
	if (!EnqueueRequest(Reason, Profile, CheckpointRecord, CheckpointActor, DisplayName))
	{
		return false;
	}

	TryStartPendingSave();
	return true;
}

bool UEMSAutosaveSubsystem::EnqueueRequest(
	FName Reason,
	const FString& Profile,
	const FEMSCheckpointRecord* CheckpointRecord,
	AEMSCheckpoint* CheckpointActor,
	const FText& DisplayName)
{
	if (Profile.IsEmpty() || (CheckpointRecord && !IsValid(CheckpointActor)))
	{
		return false;
	}

	FName CanceledReason;
	if (PendingRequest.IsPending()
		&& (!PendingRequest.IsForSlot(Profile)
			|| (CheckpointRecord && PendingRequest.IsCheckpoint()
				&& PendingCheckpointActor.Get() != CheckpointActor)))
	{
		CanceledReason = PendingRequest.GetReason();
		CancelPendingRequest(TEXT("Pending autosave was superseded by a new request."), false);
	}

	if (CheckpointRecord)
	{
		PendingCheckpointActor = CheckpointActor;
		CheckpointActor->MarkActivationPending();
	}

	PendingRequest.Enqueue(Reason, Profile, CheckpointRecord, DisplayName);
	// Publish the replacement before user code can retry the canceled checkpoint.
	// A nested request may supersede this one, but nothing overwrites it afterward.
	if (!CanceledReason.IsNone())
	{
		OnAutosaveFailed.Broadcast(CanceledReason);
	}
	return true;
}

bool UEMSAutosaveSubsystem::IsLevelStreamingBlockingSave() const
{
	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	UEMSObject* EMS = UEMSObject::Get(this);
	if (!Settings || !EMS || !EMSFLAG::IsLevel(Settings->SaveDataFlags) || !EMS->HasAnyLevelStreaming())
	{
		return false;
	}

	const bool bWorldPartitionBlocked = EMS->AutoSaveLoadWorldPartition() && !EMS->WorldPartitionLoadComplete() && !UEMSObject::SkipInitialWorldPartitionLoad();
	const bool bStreamingBlocked = !EMS->AutoSaveLoadWorldPartition() && EMS->IsLevelStreaming();
	return bWorldPartitionBlocked || bStreamingBlocked;
}

bool UEMSAutosaveSubsystem::CanStartSave(
	const FString& RequestedProfile,
	float& OutRetryDelay) const
{
	OutRetryDelay = -1.0f;
	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	UWorld* World = GetWorld();
	if (!Settings
		|| bIsShuttingDown
		|| !World
		|| !World->IsGameWorld()
		|| World->bIsTearingDown
		|| !EMSAddons::HasPersistenceAuthority(World->GetNetMode()))
	{
		return false;
	}
	if (!Settings->bEnableAutosave && !PendingRequest.IsCheckpoint())
	{
		return false;
	}
	if (IsAutosaveBlocked())
	{
		return false;
	}

	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS
		|| RequestedProfile.IsEmpty()
		|| !GetSaveProfile().Equals(RequestedProfile, ESearchCase::IgnoreCase))
	{
		return false;
	}

	// An active EMS task also blocks the slot switch the save needs.
	if (FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_All, false) || EMS->IsAsyncTaskActive(false))
	{
		OutRetryDelay = 0.1f;
		return false;
	}

	if (IsLevelStreamingBlockingSave())
	{
		OutRetryDelay = 0.1f;
		return false;
	}

	const float MinimumIntervalDelay = FEMSAutosavePendingRequest::GetMinimumIntervalDelay(
		LastSuccessfulSaveSeconds,
		FPlatformTime::Seconds(),
		Settings->MinimumTimeBetweenSaves);
	if (MinimumIntervalDelay > 0.0f)
	{
		OutRetryDelay = MinimumIntervalDelay;
		return false;
	}
	return true;
}

void UEMSAutosaveSubsystem::TryStartPendingSave()
{
	if (!PendingRequest.IsPending() || bSaveActive)
	{
		return;
	}

	// A load changes the state a pending request was meant to capture. Never carry
	// an addon autosave across an EMS load and then save immediately after restore.
	if (IsCheckpointLoadInProgress()
		|| IsSaveLoadInProgress()
		|| FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_All, false))
	{
		CancelPendingRequest(TEXT("Pending autosave canceled because an EMS load started."), false);
		return;
	}

	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS || !PendingRequest.IsForSlot(GetSaveProfile()))
	{
		CancelPendingRequest(TEXT("Pending autosave canceled because its save profile is no longer active."), true);
		return;
	}

	float RetryDelay = -1.0f;
	if (!CanStartSave(PendingRequest.GetSaveSlot(), RetryDelay))
	{
		if (RetryDelay >= 0.0f)
		{
			ScheduleRetry(RetryDelay);
		}
		return;
	}

	FName Reason;
	FString Profile;
	bool bIsCheckpoint = false;
	FEMSCheckpointRecord Checkpoint;
	FText DisplayName;
	PendingRequest.Consume(Reason, Profile, bIsCheckpoint, Checkpoint, &DisplayName);
	AEMSCheckpoint* CheckpointActor = bIsCheckpoint ? PendingCheckpointActor.Get() : nullptr;
	PendingCheckpointActor.Reset();
	const FString TargetSlot = bIsCheckpoint ? Checkpoint.SaveSlot : ResolveAutosaveTarget(Profile);
	StartSave(Reason, Profile, TargetSlot, bIsCheckpoint ? &Checkpoint : nullptr, CheckpointActor, DisplayName);
}

void UEMSAutosaveSubsystem::ScheduleRetry(const float DelaySeconds)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RetryTimer);
		World->GetTimerManager().SetTimer(RetryTimer, this, &UEMSAutosaveSubsystem::TryStartPendingSave, FMath::Max(0.01f, DelaySeconds), false);
	}
}

void UEMSAutosaveSubsystem::StartSave(
	FName Reason,
	const FString& Profile,
	const FString& TargetSlot,
	const FEMSCheckpointRecord* CheckpointRecord,
	AEMSCheckpoint* CheckpointActor,
	const FText& DisplayName,
	const EEMSSaveSlotType SaveType)
{
	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	UEMSObject* EMS = UEMSObject::Get(this);
	const auto FailBeforeStart = [this, Reason, CheckpointActor](const TCHAR* Message)
	{
		if (IsValid(CheckpointActor))
		{
			CheckpointActor->MarkActivationFailed();
		}
		if (Message)
		{
			LogMessage(Message, true);
		}
		OnAutosaveFailed.Broadcast(Reason);
	};

	if (!Settings
		|| !EMS
		|| TargetSlot.IsEmpty()
		|| !GetSaveProfile().Equals(Profile, ESearchCase::IgnoreCase))
	{
		FailBeforeStart(nullptr);
		return;
	}

	// Carried custom saves (and the checkpoint record) go with the progress into the
	// target before it becomes current, so the game reads current data from it.
	// EMS actor saves never write custom saves, so the save itself cannot overwrite them.
	// A checkpoint slot keeps its own record until the checkpoint commits a new one.
	const FString PreviousSlot = EMS->GetCurrentSaveGameName();

	// The target may hold a checkpoint's save (an earlier save of this checkpoint slot, or
	// a 1.1 checkpoint in the profile slot). Retire that record before anything is written,
	// so an interrupted overwrite can never be loaded as that checkpoint. A checkpoint save
	// writes its new record after success.
	if (!InvalidateCheckpointStoredIn(TargetSlot, false))
	{
		FailBeforeStart(TEXT("The save was not started because the checkpoint record in its target slot could not be cleared."));
		return;
	}
	if (!CarryCustomFiles(PreviousSlot, TargetSlot, CheckpointRecord == nullptr))
	{
		FailBeforeStart(TEXT("The save was not started because its carried custom files could not be copied into the target slot."));
		return;
	}
	// EMS stores the new current slot right away. Until the save ends, the journal names
	// the slot it left, so a game that ends during the save does not continue from it.
	BeginSaveJournal(TargetSlot, PreviousSlot);
	// With streaming, the target's stale Level data is removed so EMS keeps restoring streamed
	// cells from memory in the new slot. Until the save writes it, the target is not loadable.
	bool bRemovedTargetLevelData = false;
	if (!SwitchToSaveSlot(TargetSlot, EEMSSlotSwitch::ContinueSession, &bRemovedTargetLevelData))
	{
		// The journal stays while the game is still on the target, so the next start recovers it.
		if (ReturnFromAbandonedSave(PreviousSlot, TargetSlot, bRemovedTargetLevelData))
		{
			ClearSaveJournal();
		}
		FailBeforeStart(TEXT("The save was not started because EMS did not switch to its target slot."));
		return;
	}
	const FString SaveSlot = EMS->GetCurrentSaveGameName();

	// EMS unpauses a paused game when it creates the task, and only the activated
	// task pauses it again. A task abandoned below is never activated.
	const bool bGameWasPaused = UGameplayStatics::IsGamePaused(this);
	ActiveSaveTask = UEMSAsyncSaveGame::AsyncSaveActors(this, Settings->SaveDataFlags);
	if (!ActiveSaveTask)
	{
		if (ReturnFromAbandonedSave(PreviousSlot, SaveSlot, bRemovedTargetLevelData))
		{
			ClearSaveJournal();
		}
		if (Reason == ManualSaveReason)
		{
			// Manual saves are never queued; the caller hears about it now.
			OnAutosaveFailed.Broadcast(Reason);
			return;
		}
		// Enqueue without starting. QueueRequest would call TryStartPendingSave,
		// which reaches StartSave again with bSaveActive still false and recurses
		// for as long as task creation keeps failing. The retry timer is the only
		// thing that may drive the next attempt.
		if (!EnqueueRequest(Reason, Profile, CheckpointRecord, CheckpointActor, DisplayName))
		{
			// A checkpoint whose actor has gone cannot be queued again. Report it
			// rather than letting the request disappear without a callback.
			if (IsValid(CheckpointActor))
			{
				CheckpointActor->MarkActivationFailed();
			}
			OnAutosaveFailed.Broadcast(Reason);
			return;
		}
		ScheduleRetry(0.1f);
		return;
	}

	const auto RestorePauseForAbandonedTask = [this, bGameWasPaused]()
	{
		if (bGameWasPaused && !UGameplayStatics::IsGamePaused(this))
		{
			UGameplayStatics::SetGamePaused(this, true);
		}
	};

	bSaveActive = true;
	ActiveReason = Reason;
	ActiveDisplayName = CheckpointRecord ? CheckpointRecord->DisplayName : DisplayName;
	ActiveSaveType = CheckpointRecord ? EEMSSaveSlotType::Checkpoint : SaveType;
	ActiveSaveSlot = SaveSlot;
	ActivePreviousSlot = PreviousSlot;
	bActiveRequestIsCheckpoint = CheckpointRecord != nullptr;
	ActiveCheckpoint = CheckpointRecord ? *CheckpointRecord : FEMSCheckpointRecord();
	ActiveCheckpointActor = CheckpointActor;

	UEMSAsyncSaveGame* const SaveTask = ActiveSaveTask;
	SaveTask->OnCompleted.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleSaveCompleted);
	SaveTask->OnFailed.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleSaveFailed);
	OnAutosaveStarted.Broadcast(ActiveReason);

	// User callbacks are arbitrary code. World cleanup can reset ActiveSaveTask,
	// and callbacks can also switch save slots, install a blocker, or start an EMS
	// operation before the unactivated task begins. Never activate stale work.
	if (!bSaveActive || ActiveSaveTask != SaveTask)
	{
		// World cleanup owns the slot in that case; a callback that started other work leaves it to that work.
		// Nothing was written. A target left without its Level data is no save any more, unless it is
		// current again (a later save writes it). EMS refuses the delete while other work runs.
		if (bRemovedTargetLevelData)
		{
			if (UEMSObject* CurrentEMS = UEMSObject::Get(this);
				CurrentEMS && !CurrentEMS->GetCurrentSaveGameName().Equals(SaveSlot, ESearchCase::IgnoreCase))
			{
				CurrentEMS->DeleteAllSaveDataForSlot(SaveSlot);
			}
		}
		RestorePauseForAbandonedTask();
		return;
	}

	UWorld* World = GetWorld();
	EMS = UEMSObject::Get(this);
	if (bIsShuttingDown
		|| !World
		|| !World->IsGameWorld()
		|| World->bIsTearingDown
		|| !EMSAddons::HasPersistenceAuthority(World->GetNetMode())
		|| !EMS
		|| !EMS->GetCurrentSaveGameName().Equals(SaveSlot, ESearchCase::IgnoreCase)
		|| EMS->IsAsyncTaskActive(false)
		|| IsCheckpointLoadInProgress()
		|| IsSaveLoadInProgress()
		|| (IsAutosaveBlocked() && Reason != ManualSaveReason))
	{
		const TWeakObjectPtr<AEMSCheckpoint> AbortedActor = ActiveCheckpointActor;
		// EMS refuses the switch back while a callback's EMS task runs. Then the journal
		// stays, so the next start returns to the previous slot.
		ResetActiveSaveState(false);
		RestorePauseForAbandonedTask();
		if (ReturnFromAbandonedSave(PreviousSlot, SaveSlot, bRemovedTargetLevelData))
		{
			ClearSaveJournal();
		}
		if (AbortedActor.IsValid())
		{
			AbortedActor->MarkActivationFailed();
		}
		LogMessage(TEXT("Autosave was canceled because its start callback invalidated the save slot, world, authority, blocker, or EMS task state."), true);
		OnAutosaveFailed.Broadcast(Reason);
		return;
	}

	SaveTask->Activate();
}

void UEMSAutosaveSubsystem::ResetActiveSaveState(const bool bClearJournal)
{
	bSaveActive = false;
	ActiveSaveTask = nullptr;
	ActiveReason = NAME_None;
	ActiveDisplayName = FText();
	ActiveSaveType = EEMSSaveSlotType::Autosave;
	ActiveSaveSlot.Reset();
	ActivePreviousSlot.Reset();
	bActiveRequestIsCheckpoint = false;
	ActiveCheckpoint = FEMSCheckpointRecord();
	ActiveCheckpointActor.Reset();
	// Every end of an addon save passes here, after any switch back to the previous slot.
	if (bClearJournal)
	{
		ClearSaveJournal();
	}
}

void UEMSAutosaveSubsystem::BeginSaveJournal(const FString& TargetSlot, const FString& PreviousSlot)
{
	const UEMSObject* EMS = UEMSObject::Get(this);
	const FString Target = FSavePaths::ValidateSaveName(TargetSlot);
	const FString Previous = FSavePaths::ValidateSaveName(PreviousSlot);
	if (!EMS || Target.IsEmpty() || Target.Equals(Previous, ESearchCase::IgnoreCase))
	{
		// Writing the current slot again leaves no other slot to return to.
		ClearSaveJournal();
		return;
	}

	InterruptedSaveUser = EMS->GetCurrentSaveUserName();
	InterruptedSaveSlot = Target;
	InterruptedSavePreviousSlot = Previous;
	SaveConfig();
}

void UEMSAutosaveSubsystem::ClearSaveJournal()
{
	if (InterruptedSaveSlot.IsEmpty() && InterruptedSavePreviousSlot.IsEmpty() && InterruptedSaveUser.IsEmpty())
	{
		return;
	}
	InterruptedSaveUser.Reset();
	InterruptedSaveSlot.Reset();
	InterruptedSavePreviousSlot.Reset();
	SaveConfig();
}

void UEMSAutosaveSubsystem::ClearJournalCompletedBy(const FString& SavedSlot, const ESaveGameMode Mode)
{
	// An addon save that is still running owns its journal and clears it when it ends.
	if (bSaveActive || ActiveSaveTask || InterruptedSaveSlot.IsEmpty())
	{
		return;
	}

	// The journal outlived its save: world teardown cut that save off while EMS still ran it.
	// A later successful EMS save of the same slot that writes all the addon's data has
	// rewritten whatever that save may have left half written, so the slot is complete and
	// the next start must not delete it as interrupted.
	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	const UEMSObject* EMS = UEMSObject::Get(this);
	if (!Settings || !EMS)
	{
		return;
	}
	const ESaveTypeFlags Flags = static_cast<ESaveTypeFlags>(Settings->SaveDataFlags);
	const bool bCoversAddonData = Mode == ESaveGameMode::MODE_All
		|| (Mode == ESaveGameMode::MODE_Player && !EnumHasAnyFlags(Flags, ESaveTypeFlags::SF_Level))
		|| (Mode == ESaveGameMode::MODE_Level && !EnumHasAnyFlags(Flags, ESaveTypeFlags::SF_Player));
	if (bCoversAddonData
		&& FSavePaths::ValidateSaveName(SavedSlot).Equals(InterruptedSaveSlot, ESearchCase::IgnoreCase)
		&& EMS->GetCurrentSaveUserName().Equals(InterruptedSaveUser, ESearchCase::IgnoreCase))
	{
		ClearSaveJournal();
	}
}

bool UEMSAutosaveSubsystem::RecoverInterruptedSave()
{
	// Another game instance of this process may have written the journal since this one was created.
	LoadConfig();
	const FString User = InterruptedSaveUser;
	const FString Slot = InterruptedSaveSlot;
	const FString Previous = InterruptedSavePreviousSlot;
	if (Slot.IsEmpty())
	{
		return false;
	}
	ClearSaveJournal();

	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS
		|| !EMS->GetCurrentSaveUserName().Equals(User, ESearchCase::IgnoreCase)
		|| !EMS->GetCurrentSaveGameName().Equals(Slot, ESearchCase::IgnoreCase))
	{
		// The game already moved on to another slot or user; the journal no longer applies.
		return false;
	}
	if (Previous.IsEmpty() || !EMS->DoesSaveSlotExist(Previous))
	{
		LogMessage(FString::Printf(TEXT("The game ended while saving %s, which may be incomplete. There is no earlier save to continue from."), *Slot), true);
		return false;
	}

	EMS->SetCurrentSaveGameName(Previous);
	if (!EMS->GetCurrentSaveGameName().Equals(Previous, ESearchCase::IgnoreCase))
	{
		LogMessage(FString::Printf(TEXT("The game ended while saving %s, and EMS could not return to %s."), *Slot, *Previous), true);
		return false;
	}

	// The slot was being overwritten, so it holds neither its old save nor a complete new one.
	EMS->DeleteAllSaveDataForSlot(Slot);
	LogMessage(FString::Printf(TEXT("The game ended while saving %s. Continuing from %s; the incomplete save was deleted."), *Slot, *Previous), true);
	return true;
}

void UEMSAutosaveSubsystem::CancelPendingRequest(const FString& Message, const bool bBroadcastFailure)
{
	if (!PendingRequest.IsPending())
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RetryTimer);
	}

	const FName CanceledReason = PendingRequest.GetReason();
	if (PendingCheckpointActor.IsValid())
	{
		PendingCheckpointActor->MarkActivationFailed();
	}
	PendingCheckpointActor.Reset();
	PendingRequest.Reset();
	LogMessage(Message, true);
	if (bBroadcastFailure && !CanceledReason.IsNone())
	{
		OnAutosaveFailed.Broadcast(CanceledReason);
	}
}

void UEMSAutosaveSubsystem::CancelPendingRequestAfterEMSLoad()
{
	if (PendingRequest.IsPending())
	{
		CancelPendingRequest(TEXT("Pending autosave canceled because EMS restored saved state."), false);
	}
}

void UEMSAutosaveSubsystem::RefreshCheckpointActorsFromSaveData()
{
	if (!GetWorld())
	{
		return;
	}

	for (TActorIterator<AEMSCheckpoint> It(GetWorld()); It; ++It)
	{
		It->RefreshTriggerCollisionFromSaveData(this);
	}
}

void UEMSAutosaveSubsystem::HandleSaveCompleted()
{
	const FName CompletedReason = ActiveReason;
	const FString CompletedSlot = ActiveSaveSlot;
	const FString PreviousSlot = ActivePreviousSlot;
	const FText CompletedDisplayName = ActiveDisplayName;
	const EEMSSaveSlotType CompletedSaveType = ActiveSaveType;
	const bool bCheckpointSave = bActiveRequestIsCheckpoint;
	const FEMSCheckpointRecord CompletedCheckpoint = ActiveCheckpoint;
	const TWeakObjectPtr<AEMSCheckpoint> CompletedActor = ActiveCheckpointActor;
	AddonSaveSlotAwaitingActorsSaved = CompletedSlot;

	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS || !EMS->GetCurrentSaveGameName().Equals(CompletedSlot, ESearchCase::IgnoreCase))
	{
		ResetActiveSaveState();
		if (CompletedActor.IsValid())
		{
			CompletedActor->MarkActivationFailed();
		}
		LogMessage(TEXT("Autosave completed after the current EMS save slot changed; its metadata was not written."), true);
		OnAutosaveFailed.Broadcast(CompletedReason);
		return;
	}

	// Keep the interrupted-save journal until the snapshot's label is durable too.
	// A metadata failure must not expose an overwritten save with its old label.
	if (!WriteSaveSlotMetadata(CompletedSlot, CompletedDisplayName, CompletedSaveType))
	{
		ResetActiveSaveState(ReturnAndDiscardFailedSave(PreviousSlot, CompletedSlot));
		if (CompletedActor.IsValid())
		{
			CompletedActor->MarkActivationFailed();
		}
		OnAutosaveFailed.Broadcast(CompletedReason);
		return;
	}
	ResetActiveSaveState();
	LastSuccessfulSaveSeconds = FPlatformTime::Seconds();
	const bool bCommitted = !bCheckpointSave || CommitCheckpoint(CompletedCheckpoint, CompletedSlot);
	if (bCommitted)
	{
		if (bCheckpointSave)
		{
			// UEMSAsyncSaveGame broadcasts OnActorsSaved immediately after this node
			// callback returns. That one save is this checkpoint's own.
			bIgnoreNextSuccessfulActorsSaved = true;
			if (CompletedActor.IsValid())
			{
				CompletedActor->MarkActivationCommitted();
			}
		}
		OnAutosaveCompleted.Broadcast(CompletedReason);
	}
	else
	{
		// Only a complete save stays current.
		ReturnToPreviousSlot(PreviousSlot, CompletedSlot);
		if (CompletedActor.IsValid())
		{
			CompletedActor->MarkActivationFailed();
		}
		OnAutosaveFailed.Broadcast(CompletedReason);
	}
}

void UEMSAutosaveSubsystem::HandleSaveFailed()
{
	const FName FailedReason = ActiveReason;
	const FString FailedSlot = ActiveSaveSlot;
	const FString PreviousSlot = ActivePreviousSlot;
	const TWeakObjectPtr<AEMSCheckpoint> FailedActor = ActiveCheckpointActor;
	AddonSaveSlotAwaitingActorsSaved = FailedSlot;

	// The failed slot may hold a partial save. Continue from the last complete one.
	ResetActiveSaveState(ReturnAndDiscardFailedSave(PreviousSlot, FailedSlot));
	if (FailedActor.IsValid())
	{
		FailedActor->MarkActivationFailed();
	}
	OnAutosaveFailed.Broadcast(FailedReason);
}

bool UEMSAutosaveSubsystem::CommitCheckpoint(
	const FEMSCheckpointRecord& Record,
	const FString& SaveSlot)
{
	if (!Record.IsValid() || SaveSlot.IsEmpty())
	{
		return false;
	}
	UEMSObject* EMS = UEMSObject::Get(this);
	UEMSCheckpointSaveGame* Save = ResolveCheckpointSave(SaveSlot);
	if (!Save
		|| !EMS
		|| !EMS->GetCurrentSaveGameName().Equals(SaveSlot, ESearchCase::IgnoreCase))
	{
		return false;
	}

	// The checkpoint slot now names itself. Later saves carry this record along.
	const bool bPreviousHasCheckpoint = Save->bHasCheckpoint;
	const FEMSCheckpointRecord PreviousRecord = Save->LatestCheckpoint;
	const FGuid PreviousSaveGeneration = Save->SaveGeneration;
	const FGuid PreviousCheckpointGeneration = Save->CheckpointGeneration;
	Save->bHasCheckpoint = true;
	Save->LatestCheckpoint = Record;
	Save->SaveGeneration.Invalidate();
	Save->CheckpointGeneration.Invalidate();
	if (!EMS->SaveCustom(Save))
	{
		Save->bHasCheckpoint = bPreviousHasCheckpoint;
		Save->LatestCheckpoint = PreviousRecord;
		Save->SaveGeneration = PreviousSaveGeneration;
		Save->CheckpointGeneration = PreviousCheckpointGeneration;
		LogMessage(TEXT("The checkpoint's EMS save succeeded, but its checkpoint record could not be written."), true);
		return false;
	}

	OnCheckpointCommitted.Broadcast(Record);
	return true;
}

bool UEMSAutosaveSubsystem::UpgradeLegacyCheckpointRecord(const FString& Slot)
{
	UEMSObject* EMS = UEMSObject::Get(this);
	UEMSCheckpointSaveGame* Save = ResolveCheckpointSave(Slot);
	if (!EMS || !EMSCheckpoint::IsLoadableLegacyRecord(Save))
	{
		return true;
	}

	FEMSCheckpointRecord Upgraded;
	EMSCheckpoint::ResolveRecord(Save, FSavePaths::ValidateSaveName(Slot), Upgraded);
	const FEMSCheckpointRecord Legacy = Save->LatestCheckpoint;
	const FGuid LegacySaveGeneration = Save->SaveGeneration;
	const FGuid LegacyCheckpointGeneration = Save->CheckpointGeneration;
	Save->LatestCheckpoint = Upgraded;
	Save->SaveGeneration.Invalidate();
	Save->CheckpointGeneration.Invalidate();
	if (!EMS->SaveCustom(Save))
	{
		Save->LatestCheckpoint = Legacy;
		Save->SaveGeneration = LegacySaveGeneration;
		Save->CheckpointGeneration = LegacyCheckpointGeneration;
		LogMessage(FString::Printf(TEXT("The 1.1 checkpoint record in %s could not be upgraded."), *Slot), true);
		return false;
	}
	return true;
}

bool UEMSAutosaveSubsystem::InvalidateCheckpointStoredIn(const FString& Slot, const bool bRefreshActors)
{
	UEMSObject* EMS = UEMSObject::Get(this);
	const FString ValidSlot = FSavePaths::ValidateSaveName(Slot);
	if (!EMS || ValidSlot.IsEmpty() || !EMS->DoesCustomSaveExist(ValidSlot, GetDefault<UEMSCheckpointSaveGame>()->SaveGameName))
	{
		return true;
	}

	UEMSCheckpointSaveGame* Save = ResolveCheckpointSave(ValidSlot);
	FEMSCheckpointRecord Record;
	if (!EMSCheckpoint::ResolveRecord(Save, ValidSlot, Record)
		|| !FSavePaths::ValidateSaveName(Record.SaveSlot).Equals(ValidSlot, ESearchCase::IgnoreCase))
	{
		return true;
	}

	Save->bHasCheckpoint = false;
	Save->LatestCheckpoint = FEMSCheckpointRecord();
	Save->SaveGeneration.Invalidate();
	Save->CheckpointGeneration.Invalidate();
	if (!EMS->SaveCustom(Save))
	{
		LogMessage(FString::Printf(TEXT("The checkpoint record in %s could not be cleared."), *ValidSlot), true);
		return false;
	}
	if (bRefreshActors)
	{
		RefreshCheckpointActorsFromSaveData();
	}
	return true;
}

void UEMSAutosaveSubsystem::ClearCheckpointOverwrittenBy(const FString& SavedSlot)
{
	InvalidateCheckpointStoredIn(SavedSlot, true);
}

UEMSCheckpointSaveGame* UEMSAutosaveSubsystem::ResolveCheckpointSave(const FString& SaveSlot)
{
	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS)
	{
		return nullptr;
	}
	const FString ResolvedSlot = SaveSlot.IsEmpty() ? EMS->GetCurrentSaveGameName() : SaveSlot;
	return Cast<UEMSCheckpointSaveGame>(
		EMS->GetCustomSave(UEMSCheckpointSaveGame::StaticClass(), ResolvedSlot, FString()));
}

FEMSCheckpointRecord UEMSAutosaveSubsystem::GetCheckpointForSlot(const FString& SaveSlot)
{
	const FString Slot = FSavePaths::ValidateSaveName(SaveSlot);
	FEMSCheckpointRecord Record;
	EMSCheckpoint::ResolveRecord(Slot.IsEmpty() ? nullptr : ResolveCheckpointSave(Slot), Slot, Record);
	return Record;
}

bool UEMSAutosaveSubsystem::HasCheckpoint()
{
	return GetCurrentCheckpoint().IsValid();
}

FEMSCheckpointRecord UEMSAutosaveSubsystem::GetCurrentCheckpoint()
{
	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS)
	{
		return FEMSCheckpointRecord();
	}

	const FString CurrentSlot = EMS->GetCurrentSaveGameName();
	const FEMSCheckpointRecord Current = GetCheckpointForSlot(CurrentSlot);
	if (!Current.IsValid() || FSavePaths::ValidateSaveName(Current.SaveSlot).Equals(CurrentSlot, ESearchCase::IgnoreCase))
	{
		return Current;
	}

	// Only while the checkpoint slot still holds this checkpoint. A later checkpoint
	// sharing the slot, or an overwrite that cleared it, ends it for this progress.
	const FEMSCheckpointRecord Stored = GetCheckpointForSlot(Current.SaveSlot);
	return Stored.IsValid() && Stored.HasSameIdentity(Current) ? Current : FEMSCheckpointRecord();
}

void UEMSAutosaveSubsystem::PrepareCheckpointActorForLoad()
{
	if (!CheckpointBeingLoaded.IsValid() || !GetWorld())
	{
		return;
	}

	for (TActorIterator<AEMSCheckpoint> It(GetWorld()); It; ++It)
	{
		if (It->MatchesCheckpointRecord(CheckpointBeingLoaded))
		{
			// This happens before the EMS async load task is even created. The
			// restored pawn therefore cannot generate a checkpoint overlap while
			// EMS is applying its saved state.
			It->DisableTriggerForCheckpointLoad();
			return;
		}
	}
}

void UEMSAutosaveSubsystem::RefreshCheckpointActorAfterLoad(const bool bLoadSucceeded)
{
	if (!CheckpointBeingLoaded.IsValid() || !GetWorld())
	{
		return;
	}

	for (TActorIterator<AEMSCheckpoint> It(GetWorld()); It; ++It)
	{
		if (!It->MatchesCheckpointRecord(CheckpointBeingLoaded))
		{
			continue;
		}

		if (bLoadSucceeded)
		{
			// Mark the restored checkpoint current before collision comes back.
			It->MarkActivationCommitted();
			It->RestoreTriggerCollision();
		}
		else
		{
			// A failed load does not make matching checkpoint metadata disappear.
			// Keep collision disabled while that checkpoint is still the committed
			// checkpoint for this slot; restore it only when no such data remains.
			It->RefreshTriggerCollisionFromSaveData(this);
		}
		return;
	}
}

void UEMSAutosaveSubsystem::AddAutosaveBlocker(FName Blocker)
{
	if (!Blocker.IsNone())
	{
		AutosaveBlockers.Add(Blocker);
	}
}

void UEMSAutosaveSubsystem::RemoveAutosaveBlocker(FName Blocker)
{
	if (!Blocker.IsNone())
	{
		AutosaveBlockers.Remove(Blocker);
	}
	if (!IsAutosaveBlocked())
	{
		TryStartPendingSave();
	}
}

bool UEMSAutosaveSubsystem::LoadLastCheckpoint()
{
	UWorld* World = GetWorld();
	if (bIsShuttingDown
		|| bSaveActive
		|| IsSaveLoadInProgress()
		|| !World
		|| !EMSAddons::HasPersistenceAuthority(World->GetNetMode())
		|| ActiveLoadTask
		|| FAsyncSaveHelpers::IsAsyncTaskActive(ESaveGameMode::MODE_All, false))
	{
		return false;
	}
	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS)
	{
		return false;
	}

	// A new explicit checkpoint load supersedes any late-stream placement left
	// behind by an earlier completed load.
	CheckpointAwaitingPlayerPlacement = FEMSCheckpointRecord();

	const FEMSCheckpointRecord Current = GetCurrentCheckpoint();
	if (!Current.IsValid())
	{
		return false;
	}

	// The checkpoint slot must still hold this checkpoint. Another checkpoint
	// sharing its Checkpoint Slot Name may have saved over it since.
	const FString CheckpointSlot = FSavePaths::ValidateSaveName(Current.SaveSlot);
	const FEMSCheckpointRecord Stored = EMS->DoesSaveSlotExist(CheckpointSlot)
		? GetCheckpointForSlot(CheckpointSlot)
		: FEMSCheckpointRecord();
	if (!Stored.IsValid() || !Stored.HasSameIdentity(Current))
	{
		LogMessage(FString::Printf(TEXT("Load Last Checkpoint refused: slot %s no longer holds this checkpoint."), *CheckpointSlot), true);
		return false;
	}

	// EMS does not start a load while the game is paused. On the current map nothing travels
	// (which would end the pause), so refuse before the slot or session data change.
	const bool bSameWorld = EMSCheckpoint::IsSameWorld(World, Stored.World);
	if (bSameWorld && World->IsPaused())
	{
		LogMessage(TEXT("Load Last Checkpoint refused: EMS cannot load while the game is paused. Unpause the game first."), true);
		return false;
	}

	// Loading wins over queued helper work. A pending request represents state
	// from before the restore and must never run immediately after the checkpoint.
	if (PendingRequest.IsPending())
	{
		CancelPendingRequest(TEXT("Pending autosave canceled because a checkpoint load started."), false);
	}

	// The normal EMS switch: the checkpoint slot is read from disk. Clearing user data
	// drops this session's multi-level and streaming memory, which belongs to other progress.
	SlotBeforeTravelLoad = EMS->GetCurrentSaveGameName();
	if (!EMS->GetCurrentSaveGameName().Equals(CheckpointSlot, ESearchCase::IgnoreCase))
	{
		EMS->SetCurrentSaveGameName(CheckpointSlot);
		EMS->ClearUserData();
		if (!EMS->GetCurrentSaveGameName().Equals(CheckpointSlot, ESearchCase::IgnoreCase))
		{
			LogMessage(FString::Printf(TEXT("Load Last Checkpoint could not make %s the current EMS slot."), *CheckpointSlot), true);
			return false;
		}
	}
	CheckpointLoadSlot = CheckpointSlot;
	CheckpointBeingLoaded = Stored;

	if (!bSameWorld)
	{
		bCheckpointLoadAfterTravel = true;
		const TSoftObjectPtr<UWorld> TargetWorld(CheckpointBeingLoaded.World);
		UGameplayStatics::OpenLevelBySoftObjectPtr(this, TargetWorld, true);
		return true;
	}

	BeginCheckpointLoad();
	return ActiveLoadTask != nullptr;
}

void UEMSAutosaveSubsystem::BeginCheckpointLoad()
{
	const bool bArrivedByTravel = bCheckpointLoadAfterTravel;
	const FString SlotBeforeLoad = SlotBeforeTravelLoad;
	bCheckpointLoadAfterTravel = false;
	SlotBeforeTravelLoad.Reset();
	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	UEMSObject* EMS = UEMSObject::Get(this);
	UWorld* World = GetWorld();
	if (!Settings
		|| !CheckpointBeingLoaded.IsValid()
		|| !EMS
		|| !World
		|| !EMSAddons::HasPersistenceAuthority(World->GetNetMode()))
	{
		FailCheckpointLoad(TEXT("The last checkpoint could not be loaded because its state is invalid."));
		return;
	}
	if (!EMS->GetCurrentSaveGameName().Equals(CheckpointLoadSlot, ESearchCase::IgnoreCase))
	{
		FailCheckpointLoad(TEXT("Checkpoint loading was canceled because the active EMS save slot changed."));
		return;
	}

	PrepareCheckpointActorForLoad();

	ActiveLoadTask = UEMSAsyncLoadGame::AsyncLoadActors(this, Settings->SaveDataFlags, true);
	if (!ActiveLoadTask)
	{
		// On the current map the old progress is still running, so it stays current.
		if (!bArrivedByTravel)
		{
			SlotBeforeTravelLoad = SlotBeforeLoad;
			ReturnFromFailedTravel(CheckpointLoadSlot);
		}
		FailCheckpointLoad(TEXT("The last checkpoint could not be loaded because EMS could not start a load operation."));
		return;
	}
	ActiveLoadTask->OnCompleted.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleLoadCompleted);
	ActiveLoadTask->OnFailed.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleLoadFailed);
	ActiveLoadTask->Activate();
}

void UEMSAutosaveSubsystem::HandleLoadCompleted()
{
	ActiveLoadTask = nullptr;
	if (!SaveBeingLoaded.IsEmpty())
	{
		CompleteSaveLoad();
		return;
	}

	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS || !EMS->GetCurrentSaveGameName().Equals(CheckpointLoadSlot, ESearchCase::IgnoreCase))
	{
		FailCheckpointLoad(TEXT("Checkpoint loading completed after the active EMS save slot changed."));
		return;
	}

	// EMS has already restored the player state, including its saved transform.
	// Completion then hands off to the matching checkpoint actor, which places the
	// player at its trigger center on the delayed arm step and keeps the restored
	// rotation. The checkpoint actor is the restore point, so moving it in the level
	// also moves where a loaded checkpoint puts the player.
	CompleteCheckpointLoad();
}

void UEMSAutosaveSubsystem::HandleLoadFailed()
{
	ActiveLoadTask = nullptr;
	if (!SaveBeingLoaded.IsEmpty())
	{
		FailSaveLoad(TEXT("EMS failed to load the save."));
		return;
	}
	FailCheckpointLoad(TEXT("EMS failed to load the last checkpoint."));
}

void UEMSAutosaveSubsystem::HandleEMSPlayerLoaded(const APlayerController* LoadedPlayer)
{
	if (LoadedPlayer)
	{
		CancelPendingRequestAfterEMSLoad();
	}
}

void UEMSAutosaveSubsystem::HandleEMSLevelLoaded(const TArray<TSoftObjectPtr<AActor>>& LoadedActors)
{
	CancelPendingRequestAfterEMSLoad();
}

void UEMSAutosaveSubsystem::HandleEMSActorsSaved(ESaveGameMode Mode, bool bSuccess)
{
	UWorld* World = GetWorld();
	if (!World || !EMSAddons::HasPersistenceAuthority(World->GetNetMode()))
	{
		return;
	}

	const FString SavedSlot = !AddonSaveSlotAwaitingActorsSaved.IsEmpty()
		? AddonSaveSlotAwaitingActorsSaved
		: (UEMSObject::Get(this) ? UEMSObject::Get(this)->GetCurrentSaveGameName() : FString());
	AddonSaveSlotAwaitingActorsSaved.Reset();

	if (!bSuccess)
	{
		bIgnoreNextSuccessfulActorsSaved = false;
		TryStartPendingSave();
		return;
	}

	LastSuccessfulSaveSeconds = FPlatformTime::Seconds();
	ClearJournalCompletedBy(SavedSlot, Mode);
	if (bIgnoreNextSuccessfulActorsSaved)
	{
		bIgnoreNextSuccessfulActorsSaved = false;
		TryStartPendingSave();
		return;
	}

	// A later successful save records where the player is now, so a late-stream
	// player-placement handoff from an earlier checkpoint load no longer applies.
	if (CheckpointAwaitingPlayerPlacement.IsValid() && !SavedSlot.IsEmpty())
	{
		CheckpointAwaitingPlayerPlacement = FEMSCheckpointRecord();
		CheckpointLoadSlot.Reset();
	}
	// The checkpoint itself stays, unless this save (for example a game's own
	// Save Game Actors right after Load Last Checkpoint) wrote over its slot.
	ClearCheckpointOverwrittenBy(SavedSlot);
	TryStartPendingSave();
}

bool UEMSAutosaveSubsystem::ConsumeCheckpointPlayerPlacement(const AEMSCheckpoint* Checkpoint)
{
	if (!Checkpoint || !CheckpointAwaitingPlayerPlacement.IsValid())
	{
		return false;
	}

	// CheckpointLoadSlot remains the owner of this handoff until it is consumed.
	// If the user changed save slots while the checkpoint was streamed out, the
	// old placement belongs to the previous slot and must never move the new one.
	if (UEMSObject* EMS = UEMSObject::Get(this);
		EMS && !EMS->GetCurrentSaveGameName().Equals(CheckpointLoadSlot, ESearchCase::IgnoreCase))
	{
		CheckpointAwaitingPlayerPlacement = FEMSCheckpointRecord();
		CheckpointLoadSlot.Reset();
		return false;
	}

	if (!Checkpoint->MatchesCheckpointRecord(CheckpointAwaitingPlayerPlacement))
	{
		return false;
	}

	// One shot. The same checkpoint streaming out and back in later must not
	// teleport the player a second time.
	CheckpointAwaitingPlayerPlacement = FEMSCheckpointRecord();
	CheckpointLoadSlot.Reset();
	return true;
}

void UEMSAutosaveSubsystem::CompleteCheckpointLoad()
{
	const FEMSCheckpointRecord LoadedCheckpoint = CheckpointBeingLoaded;

	// Recorded before the broadcast. A checkpoint that is already loaded claims
	// this during RefreshCheckpointActorAfterLoad or its On Checkpoint Loaded
	// handler; one that streams in later claims it from Begin Play.
	CheckpointAwaitingPlayerPlacement = LoadedCheckpoint;

	RefreshCheckpointActorAfterLoad(true);
	CheckpointBeingLoaded = FEMSCheckpointRecord();
	// Keep CheckpointLoadSlot until the matching checkpoint consumes the
	// one-shot placement. It also binds a late-stream handoff to its save slot.
	bCheckpointLoadAfterTravel = false;
	OnCheckpointLoaded.Broadcast(LoadedCheckpoint);
}

void UEMSAutosaveSubsystem::FailCheckpointLoad(const FString& Message)
{
	const FEMSCheckpointRecord FailedCheckpoint = CheckpointBeingLoaded;
	RefreshCheckpointActorAfterLoad(false);
	ActiveLoadTask = nullptr;
	CheckpointBeingLoaded = FEMSCheckpointRecord();
	CheckpointLoadSlot.Reset();
	bCheckpointLoadAfterTravel = false;
	SlotBeforeTravelLoad.Reset();
	LogMessage(Message, true);
	if (FailedCheckpoint.IsValid())
	{
		OnCheckpointLoadFailed.Broadcast(FailedCheckpoint);
	}
}

void UEMSAutosaveSubsystem::HandlePostLoadMap(UWorld* LoadedWorld)
{
	if (bIsShuttingDown || !LoadedWorld || !LoadedWorld->IsGameWorld() || LoadedWorld->GetGameInstance() != GetGameInstance())
	{
		return;
	}
	ConfigureWorldTimers(LoadedWorld);
	if (bSaveLoadAfterTravel)
	{
		BeginSaveLoad(LoadedWorld);
		return;
	}
	if (bCheckpointLoadAfterTravel)
	{
		UEMSObject* EMS = UEMSObject::Get(this);
		if (!EMS || !EMS->GetCurrentSaveGameName().Equals(CheckpointLoadSlot, ESearchCase::IgnoreCase))
		{
			FailCheckpointLoad(TEXT("Checkpoint travel completed after the active EMS save slot changed."));
		}
		else if (EMSCheckpoint::IsSameWorld(LoadedWorld, CheckpointBeingLoaded.World))
		{
			BeginCheckpointLoad();
		}
		else
		{
			FailCheckpointLoad(TEXT("Checkpoint travel completed in a world that does not match the checkpoint record."));
		}
		return;
	}
	TryStartPendingSave();
}

void UEMSAutosaveSubsystem::HandleWorldCleanup(
	UWorld* World,
	bool bSessionEnded,
	bool bCleanupResources)
{
	if (!World || !World->IsGameWorld() || World->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	World->GetTimerManager().ClearAllTimersForObject(this);
	AutosaveBlockers.Empty();
	AddonSaveSlotAwaitingActorsSaved.Reset();
	bIgnoreNextSuccessfulActorsSaved = false;

	// A completed load may still be waiting for its checkpoint cell to appear.
	// Leaving this world ends that placement request. During checkpoint travel
	// the handoff does not exist yet, so CheckpointLoadSlot is preserved below.
	if (CheckpointAwaitingPlayerPlacement.IsValid())
	{
		CheckpointAwaitingPlayerPlacement = FEMSCheckpointRecord();
		CheckpointLoadSlot.Reset();
	}

	if (bSaveActive || ActiveSaveTask)
	{
		const FName InterruptedReason = ActiveReason;
		const FString InterruptedSlot = ActiveSaveSlot;
		const FString PreviousSlot = ActivePreviousSlot;
		const TWeakObjectPtr<AEMSCheckpoint> InterruptedActor = ActiveCheckpointActor;
		// The interrupted slot may hold a partial save. Continue from the last complete one.
		// If EMS still runs the task and refuses, the journal stays for the next start.
		// The slot is kept, never deleted: EMS reports a save's result one tick after it
		// has written everything, on this world's timer, so a save that finished in the
		// frame this world was torn down never reports at all and may well be complete.
		ResetActiveSaveState(ReturnToPreviousSlot(PreviousSlot, InterruptedSlot, EEMSSlotSwitch::LeaveWorld));
		if (InterruptedActor.IsValid())
		{
			InterruptedActor->MarkActivationFailed();
		}
		if (!InterruptedReason.IsNone())
		{
			OnAutosaveFailed.Broadcast(InterruptedReason);
		}
		LogMessage(TEXT("Autosave state was cleared because its world was cleaned up before the EMS task completed."), true);
	}

	if (PendingRequest.IsPending())
	{
		CancelPendingRequest(TEXT("Pending autosave canceled because its world was cleaned up."), true);
	}

	ActiveLoadTask = nullptr;
	if (!bCheckpointLoadAfterTravel && CheckpointBeingLoaded.IsValid())
	{
		const FEMSCheckpointRecord InterruptedCheckpoint = CheckpointBeingLoaded;
		CheckpointBeingLoaded = FEMSCheckpointRecord();
		CheckpointLoadSlot.Reset();
		OnCheckpointLoadFailed.Broadcast(InterruptedCheckpoint);
		LogMessage(TEXT("Checkpoint loading was interrupted by world cleanup."), true);
	}

	// Leaving the world on the way to the save's map is expected. Losing the
	// world while its load is running is not.
	if (!bSaveLoadAfterTravel && !SaveBeingLoaded.IsEmpty())
	{
		FailSaveLoad(TEXT("Save loading was interrupted by world cleanup."));
	}
}

void UEMSAutosaveSubsystem::ConfigureWorldTimers(UWorld* World)
{
	if (!World || !EMSAddons::HasPersistenceAuthority(World->GetNetMode()))
	{
		return;
	}
	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	if (!Settings)
	{
		return;
	}

	World->GetTimerManager().ClearTimer(PeriodicTimer);
	World->GetTimerManager().ClearTimer(MapLoadTimer);
	const float PeriodicInterval = ResolvePeriodicAutosaveInterval();
	if (Settings->bEnableAutosave && PeriodicInterval > 0.0f)
	{
		World->GetTimerManager().SetTimer(PeriodicTimer, this, &UEMSAutosaveSubsystem::HandlePeriodicAutosave, PeriodicInterval, true);
	}
	if (Settings->bEnableAutosave
		&& Settings->bAutosaveAfterMapLoad
		&& !bCheckpointLoadAfterTravel
		&& !bSaveLoadAfterTravel)
	{
		World->GetTimerManager().SetTimer(MapLoadTimer, this, &UEMSAutosaveSubsystem::HandleMapLoadAutosave, FMath::Max(0.01f, Settings->MapLoadAutosaveDelay), false);
	}
}

void UEMSAutosaveSubsystem::HandlePeriodicAutosave()
{
	RequestAutosave(PeriodicAutosaveReason);
}

void UEMSAutosaveSubsystem::HandleMapLoadAutosave()
{
	UWorld* World = GetWorld();
	if (bIsShuttingDown
		|| IsCheckpointLoadInProgress()
		|| IsSaveLoadInProgress()
		|| !World
		|| !World->IsGameWorld()
		|| World->bIsTearingDown
		|| !EMSAddons::HasPersistenceAuthority(World->GetNetMode()))
	{
		return;
	}

	if (FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_All, false))
	{
		World->GetTimerManager().SetTimer(
			MapLoadTimer,
			this,
			&UEMSAutosaveSubsystem::HandleMapLoadAutosave,
			MapLoadAutosaveRetryDelay,
			false);
		return;
	}

	RequestAutosave(MapLoadAutosaveReason);
}

void UEMSAutosaveSubsystem::LogMessage(const FString& Message, const bool bWarning) const
{
	if (bWarning)
	{
		UE_LOG(LogEMSAddonsAutosave, Warning, TEXT("%s"), *Message);
	}
	else
	{
		UE_LOG(LogEMSAddonsAutosave, Log, TEXT("%s"), *Message);
	}
}
