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
	constexpr float MapLoadAutosaveRetryDelay = 0.1f;
}

void UEMSAutosaveSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UEMSObject>();
	bIsShuttingDown = false;
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UEMSAutosaveSubsystem::HandlePostLoadMap);
	FWorldDelegates::OnWorldCleanup.AddUObject(this, &UEMSAutosaveSubsystem::HandleWorldCleanup);

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UEMSObject* EMS = GameInstance->GetSubsystem<UEMSObject>())
		{
			EMS->OnPlayerLoaded.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleEMSPlayerLoaded);
			EMS->OnLevelLoaded.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleEMSLevelLoaded);
			EMS->OnActorsSaved.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleEMSActorsSaved);
		}
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
	AutosaveBlockers.Empty();
	PendingRequest.Reset();
	PendingCheckpointActor.Reset();
	ResetActiveSaveState();
	ActiveLoadTask = nullptr;
	CheckpointBeingLoaded = FEMSCheckpointRecord();
	CheckpointAwaitingPlayerPlacement = FEMSCheckpointRecord();
	CheckpointLoadSlot.Reset();
	AddonSaveSlotAwaitingActorsSaved.Reset();
	bCheckpointLoadAfterTravel = false;
	bIgnoreNextSuccessfulActorsSaved = false;
	Super::Deinitialize();
}

bool UEMSAutosaveSubsystem::RequestAutosave(FName Reason)
{
	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	if (!Settings
		|| !Settings->bEnableAutosave
		|| bIsShuttingDown
		|| IsCheckpointLoadInProgress()
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

	const FString SaveSlot = EMS->GetCurrentSaveGameName();
	return !SaveSlot.IsEmpty()
		&& QueueRequest(Reason.IsNone() ? DefaultAutosaveReason : Reason, SaveSlot, nullptr, nullptr);
}

bool UEMSAutosaveSubsystem::ActivateCheckpoint(AEMSCheckpoint* Checkpoint)
{
	if (bIsShuttingDown
		|| IsCheckpointLoadInProgress()
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
	const FString SaveSlot = EMS->GetCurrentSaveGameName();
	if (SaveSlot.IsEmpty())
	{
		return false;
	}

	FEMSCheckpointRecord Record;
	Record.CheckpointId = Checkpoint->CheckpointId;
	Record.DisplayName = Checkpoint->DisplayName;
	Record.World = EMSCheckpoint::GetWorldAssetPath(Checkpoint->GetWorld());
	Record.ActivatedAt = FDateTime::UtcNow();
	if (!Record.IsValid())
	{
		return false;
	}

	// Close re-entrancy before Blueprint callbacks can activate this actor again.
	Checkpoint->MarkActivationPending();

	const FEMSCheckpointRecord Existing = GetCheckpointForSlot(SaveSlot);
	if (Existing.IsValid()
		&& Existing.HasSameIdentity(Record)
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
	if (!EnqueueRequest(CheckpointAutosaveReason, SaveSlot, &Record, Checkpoint))
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
	const FString& SaveSlot,
	const FEMSCheckpointRecord* CheckpointRecord,
	AEMSCheckpoint* CheckpointActor)
{
	if (!EnqueueRequest(Reason, SaveSlot, CheckpointRecord, CheckpointActor))
	{
		return false;
	}

	TryStartPendingSave();
	return true;
}

bool UEMSAutosaveSubsystem::EnqueueRequest(
	FName Reason,
	const FString& SaveSlot,
	const FEMSCheckpointRecord* CheckpointRecord,
	AEMSCheckpoint* CheckpointActor)
{
	if (SaveSlot.IsEmpty())
	{
		return false;
	}

	if (PendingRequest.IsPending() && !PendingRequest.IsForSlot(SaveSlot))
	{
		CancelPendingRequest(TEXT("Pending autosave canceled because the active EMS save slot changed."), true);
	}

	if (CheckpointRecord)
	{
		if (!IsValid(CheckpointActor))
		{
			return false;
		}
		if (PendingRequest.IsCheckpoint()
			&& PendingCheckpointActor.IsValid()
			&& PendingCheckpointActor.Get() != CheckpointActor)
		{
			PendingCheckpointActor->MarkActivationFailed();
			OnAutosaveFailed.Broadcast(PendingRequest.GetReason());
		}
		PendingCheckpointActor = CheckpointActor;
		CheckpointActor->MarkActivationPending();
	}

	PendingRequest.Enqueue(Reason, SaveSlot, CheckpointRecord);
	return true;
}

bool UEMSAutosaveSubsystem::CanStartSave(
	const FString& RequestedSlot,
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
		|| RequestedSlot.IsEmpty()
		|| !EMS->GetCurrentSaveGameName().Equals(RequestedSlot, ESearchCase::IgnoreCase))
	{
		return false;
	}

	if (FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_All, false) || EMS->IsAsyncTaskActive(false))
	{
		OutRetryDelay = 0.1f;
		return false;
	}

	if (EMSFLAG::IsLevel(Settings->SaveDataFlags) && EMS->HasAnyLevelStreaming())
	{
		const bool bWorldPartitionBlocked = EMS->AutoSaveLoadWorldPartition() && !EMS->WorldPartitionLoadComplete() && !UEMSObject::SkipInitialWorldPartitionLoad();
		const bool bStreamingBlocked = !EMS->AutoSaveLoadWorldPartition() && EMS->IsLevelStreaming();
		if (bWorldPartitionBlocked || bStreamingBlocked)
		{
			OutRetryDelay = 0.1f;
			return false;
		}
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
	if (IsCheckpointLoadInProgress() || FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_All, false))
	{
		CancelPendingRequest(TEXT("Pending autosave canceled because an EMS load started."), false);
		return;
	}

	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS || !PendingRequest.IsForSlot(EMS->GetCurrentSaveGameName()))
	{
		CancelPendingRequest(TEXT("Pending autosave canceled because its EMS save slot is no longer active."), true);
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
	FString SaveSlot;
	bool bIsCheckpoint = false;
	FEMSCheckpointRecord Checkpoint;
	PendingRequest.Consume(Reason, SaveSlot, bIsCheckpoint, Checkpoint);
	AEMSCheckpoint* CheckpointActor = bIsCheckpoint ? PendingCheckpointActor.Get() : nullptr;
	PendingCheckpointActor.Reset();
	StartSave(Reason, SaveSlot, bIsCheckpoint ? &Checkpoint : nullptr, CheckpointActor);
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
	const FString& SaveSlot,
	const FEMSCheckpointRecord* CheckpointRecord,
	AEMSCheckpoint* CheckpointActor)
{
	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	UEMSObject* EMS = UEMSObject::Get(this);
	if (!Settings
		|| !EMS
		|| !EMS->GetCurrentSaveGameName().Equals(SaveSlot, ESearchCase::IgnoreCase))
	{
		if (IsValid(CheckpointActor))
		{
			CheckpointActor->MarkActivationFailed();
		}
		OnAutosaveFailed.Broadcast(Reason);
		return;
	}

	// Reserved before the EMS save so the metadata already records which save it
	// is waiting for. A reservation that cannot be written leaves the previous
	// checkpoint untouched and the EMS save unstarted.
	FGuid ReservedGeneration;
	if (CheckpointRecord && !BeginCheckpointGeneration(SaveSlot, ReservedGeneration))
	{
		if (IsValid(CheckpointActor))
		{
			CheckpointActor->MarkActivationFailed();
		}
		LogMessage(TEXT("Checkpoint activation was abandoned because its save generation could not be reserved; the previous checkpoint is unchanged."), true);
		OnAutosaveFailed.Broadcast(Reason);
		return;
	}

	ActiveSaveTask = UEMSAsyncSaveGame::AsyncSaveActors(this, Settings->SaveDataFlags);
	if (!ActiveSaveTask)
	{
		if (ReservedGeneration.IsValid())
		{
			RollbackCheckpointGeneration(SaveSlot);
		}
		// Enqueue without starting. QueueRequest would call TryStartPendingSave,
		// which reaches StartSave again with bSaveActive still false and recurses
		// for as long as task creation keeps failing. The retry timer is the only
		// thing that may drive the next attempt.
		EnqueueRequest(Reason, SaveSlot, CheckpointRecord, CheckpointActor);
		ScheduleRetry(0.1f);
		return;
	}

	bSaveActive = true;
	ActiveReason = Reason;
	ActiveSaveSlot = SaveSlot;
	bActiveRequestIsCheckpoint = CheckpointRecord != nullptr;
	ActiveCheckpoint = CheckpointRecord ? *CheckpointRecord : FEMSCheckpointRecord();
	ActiveCheckpointGeneration = ReservedGeneration;
	ActiveCheckpointActor = CheckpointActor;
	ActiveSaveTask->OnCompleted.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleSaveCompleted);
	ActiveSaveTask->OnFailed.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleSaveFailed);
	OnAutosaveStarted.Broadcast(ActiveReason);
	ActiveSaveTask->Activate();
}

void UEMSAutosaveSubsystem::ResetActiveSaveState()
{
	bSaveActive = false;
	ActiveSaveTask = nullptr;
	ActiveReason = NAME_None;
	ActiveSaveSlot.Reset();
	bActiveRequestIsCheckpoint = false;
	ActiveCheckpoint = FEMSCheckpointRecord();
	ActiveCheckpointGeneration.Invalidate();
	ActiveCheckpointActor.Reset();
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

void UEMSAutosaveSubsystem::ClearCheckpointAfterSupersedingSave(const FString& SaveSlot)
{
	if (SaveSlot.IsEmpty())
	{
		return;
	}

	UEMSObject* EMS = UEMSObject::Get(this);
	UEMSCheckpointSaveGame* Save = ResolveCheckpointSave(SaveSlot);
	if (!EMS || !Save || !EMSCheckpoint::IsCurrentGeneration(Save))
	{
		return;
	}

	if (!EMSCheckpoint::ClearCurrentCheckpoint(
		Save,
		[EMS, Save]() { return EMS->SaveCustom(Save); }))
	{
		LogMessage(TEXT("A later EMS actor save superseded the checkpoint, but its checkpoint metadata could not be cleared."), true);
		return;
	}

	if (EMS->GetCurrentSaveGameName().Equals(SaveSlot, ESearchCase::IgnoreCase))
	{
		RefreshCheckpointActorsFromSaveData();
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
	const bool bCheckpointSave = bActiveRequestIsCheckpoint;
	const FEMSCheckpointRecord CompletedCheckpoint = ActiveCheckpoint;
	const FGuid CompletedGeneration = ActiveCheckpointGeneration;
	const TWeakObjectPtr<AEMSCheckpoint> CompletedActor = ActiveCheckpointActor;
	AddonSaveSlotAwaitingActorsSaved = CompletedSlot;
	ResetActiveSaveState();

	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS || !EMS->GetCurrentSaveGameName().Equals(CompletedSlot, ESearchCase::IgnoreCase))
	{
		if (CompletedActor.IsValid())
		{
			CompletedActor->MarkActivationFailed();
		}
		LogMessage(TEXT("Autosave completed after the active EMS save slot changed; checkpoint metadata was not committed and the reserved generation remains stale."), true);
		OnAutosaveFailed.Broadcast(CompletedReason);
		return;
	}

	LastSuccessfulSaveSeconds = FPlatformTime::Seconds();
	const bool bCommitted = !bCheckpointSave
		|| CommitCheckpoint(CompletedCheckpoint, CompletedSlot, CompletedGeneration);
	if (bCommitted)
	{
		if (bCheckpointSave)
		{
			// UEMSAsyncSaveGame broadcasts OnActorsSaved immediately after this node
			// callback returns. Consume that one save without clearing the checkpoint
			// that was just paired with it.
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
	const bool bCheckpointSave = bActiveRequestIsCheckpoint;
	const TWeakObjectPtr<AEMSCheckpoint> FailedActor = ActiveCheckpointActor;
	AddonSaveSlotAwaitingActorsSaved = FailedSlot;
	ResetActiveSaveState();
	if (FailedActor.IsValid())
	{
		FailedActor->MarkActivationFailed();
	}
	if (bCheckpointSave)
	{
		// The EMS task already started and may have written one of its separate
		// core files. Keep the reserved generation unresolved so the old checkpoint
		// can never be presented as matching uncertain slot state.
		LogMessage(TEXT("Checkpoint EMS save failed after starting; the stored checkpoint is treated as stale until the next successful checkpoint."), true);
	}
	OnAutosaveFailed.Broadcast(FailedReason);
}

bool UEMSAutosaveSubsystem::BeginCheckpointGeneration(
	const FString& SaveSlot,
	FGuid& OutGeneration)
{
	OutGeneration.Invalidate();
	UEMSObject* EMS = UEMSObject::Get(this);
	UEMSCheckpointSaveGame* Save = ResolveCheckpointSave(SaveSlot);
	if (!Save || !EMS)
	{
		return false;
	}

	const FGuid Generation = FGuid::NewGuid();
	if (!EMSCheckpoint::BeginGeneration(
		Save,
		Generation,
		[EMS, Save]() { return EMS->SaveCustom(Save); }))
	{
		return false;
	}

	OutGeneration = Generation;
	return true;
}

void UEMSAutosaveSubsystem::RollbackCheckpointGeneration(const FString& SaveSlot)
{
	UEMSObject* EMS = UEMSObject::Get(this);
	UEMSCheckpointSaveGame* Save = ResolveCheckpointSave(SaveSlot);
	if (!Save || !EMS)
	{
		return;
	}

	if (!EMSCheckpoint::AbandonGeneration(
		Save,
		[EMS, Save]() { return EMS->SaveCustom(Save); }))
	{
		// The EMS task never started, but if rollback itself cannot be written the
		// unresolved generation is still the safe fallback and remains stale.
		LogMessage(TEXT("The unstarted checkpoint generation could not be rolled back; the stored checkpoint is treated as stale."), true);
	}
}

bool UEMSAutosaveSubsystem::CommitCheckpoint(
	const FEMSCheckpointRecord& Record,
	const FString& SaveSlot,
	const FGuid& Generation)
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

	if (!EMSCheckpoint::CommitRecord(
		Save,
		Record,
		Generation,
		[EMS, Save]() { return EMS->SaveCustom(Save); }))
	{
		LogMessage(TEXT("EMS actor save succeeded, but checkpoint metadata could not be committed; the stored checkpoint no longer matches the saved state and is ignored."), true);
		return false;
	}

	OnCheckpointCommitted.Broadcast(Record);
	return true;
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
	const UEMSCheckpointSaveGame* Save = ResolveCheckpointSave(SaveSlot);
	if (EMSCheckpoint::IsCurrentGeneration(Save))
	{
		return Save->LatestCheckpoint;
	}

	// Once per stale generation. Blueprint-pure accessors can reach this as often
	// as a UI binding asks, so do not emit the same warning every frame.
	if (Save && Save->bHasCheckpoint && ReportedStaleGeneration != Save->SaveGeneration)
	{
		ReportedStaleGeneration = Save->SaveGeneration;
		LogMessage(TEXT("The stored checkpoint was written for a different save generation than the slot holds and is ignored."), true);
	}
	return FEMSCheckpointRecord();
}

bool UEMSAutosaveSubsystem::HasCheckpoint()
{
	return GetCurrentCheckpoint().IsValid();
}

FEMSCheckpointRecord UEMSAutosaveSubsystem::GetCurrentCheckpoint()
{
	UEMSObject* EMS = UEMSObject::Get(this);
	return EMS ? GetCheckpointForSlot(EMS->GetCurrentSaveGameName()) : FEMSCheckpointRecord();
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
	CheckpointLoadSlot = EMS->GetCurrentSaveGameName();
	CheckpointBeingLoaded = GetCheckpointForSlot(CheckpointLoadSlot);
	if (CheckpointLoadSlot.IsEmpty() || !CheckpointBeingLoaded.IsValid())
	{
		CheckpointLoadSlot.Reset();
		CheckpointBeingLoaded = FEMSCheckpointRecord();
		return false;
	}

	// Loading wins over queued helper work. A pending request represents state
	// from before the restore and must never run immediately after the checkpoint.
	if (PendingRequest.IsPending())
	{
		CancelPendingRequest(TEXT("Pending autosave canceled because a checkpoint load started."), false);
	}

	if (!EMSCheckpoint::IsSameWorld(World, CheckpointBeingLoaded.World))
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
	bCheckpointLoadAfterTravel = false;
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
	if (bIgnoreNextSuccessfulActorsSaved)
	{
		bIgnoreNextSuccessfulActorsSaved = false;
		TryStartPendingSave();
		return;
	}

	// Any later successful normal Save Game Actors operation supersedes both the
	// checkpoint metadata and a late-stream player-placement handoff from that
	// same slot.
	if (CheckpointAwaitingPlayerPlacement.IsValid()
		&& SavedSlot.Equals(CheckpointLoadSlot, ESearchCase::IgnoreCase))
	{
		CheckpointAwaitingPlayerPlacement = FEMSCheckpointRecord();
		CheckpointLoadSlot.Reset();
	}
	ClearCheckpointAfterSupersedingSave(SavedSlot);
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
		const bool bCheckpointSave = bActiveRequestIsCheckpoint;
		const TWeakObjectPtr<AEMSCheckpoint> InterruptedActor = ActiveCheckpointActor;
		ResetActiveSaveState();
		if (InterruptedActor.IsValid())
		{
			InterruptedActor->MarkActivationFailed();
		}
		if (!InterruptedReason.IsNone())
		{
			OnAutosaveFailed.Broadcast(InterruptedReason);
		}
		LogMessage(TEXT("Autosave state was cleared because its world was cleaned up before the EMS task completed."), true);
		if (bCheckpointSave)
		{
			// The reservation is deliberately left standing. A task that never
			// reported may or may not have written the slot, and an unresolved
			// generation makes the stored checkpoint stale rather than letting it
			// describe state it might not belong to.
			LogMessage(TEXT("A checkpoint save was interrupted by world cleanup; the stored checkpoint is treated as stale until the next successful checkpoint."), true);
		}
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
	if (Settings->bEnableAutosave && Settings->bAutosaveAfterMapLoad && !bCheckpointLoadAfterTravel)
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
