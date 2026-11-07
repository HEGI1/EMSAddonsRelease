//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAutosaveSubsystem.h"

#include "Algo/AllOf.h"
#include "EMSAddonsAuthority.h"
#include "EMSAsyncLoadGame.h"
#include "EMSAutosaveSettings.h"
#include "EMSCheckpointSaveGame.h"
#include "EMSInfoSaveGame.h"
#include "EMSLevel.h"
#include "EMSMisc.h"
#include "EMSObject.h"
#include "EMSObjectAdv.h"
#include "EMSPaths.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"

/*
Save slots are ordinary EMS slots of one profile:

  <Profile>                          autosave with Autosave Slot Count 0 (1.1 behavior)
  <Profile>_Autosave_<0..N-1>        rotating autosaves, oldest replaced first
  <Profile>_Manual_<Name>            Manual Save
  <Profile>_Checkpoint[_<Name>]      checkpoint saves, named on the checkpoint actor

A save switches the current EMS slot to its target and runs a normal EMS save there.
EMS keeps its multi-level memory across any switch, so the new slot receives the complete
session. Streaming memory (World Partition cells, streamed sub-levels) is only kept by
EMS 1.86 when the new slot has no Level data; switching to a slot with Level data stops
memory restoration until the next load, like EMS 1.85. A save therefore removes the stale
Level data of the slot it replaces before switching to it. Loading switches the slot the
normal way, which makes EMS read the slot from disk.
*/

namespace
{
	/** How EMS stores a fixed name part on this platform. Console file systems replace '_'. */
	FString StoredPart(const TCHAR* RawPart)
	{
		const FString Wrapped = FSavePaths::ValidateSaveName(FString(TEXT("A")) + RawPart + TEXT("A"));
		return Wrapped.Len() > 2 ? Wrapped.Mid(1, Wrapped.Len() - 2) : FString();
	}

	/** Leftmost position where Marker is followed by a remainder Accept takes, or INDEX_NONE. */
	int32 FindMarker(const FString& Name, const FString& Marker, TFunctionRef<bool(const FString&)> Accept)
	{
		if (Marker.IsEmpty())
		{
			return INDEX_NONE;
		}

		int32 SearchFrom = 1;
		while (SearchFrom < Name.Len())
		{
			const int32 Found = Name.Find(Marker, ESearchCase::IgnoreCase, ESearchDir::FromStart, SearchFrom);
			if (Found == INDEX_NONE)
			{
				return INDEX_NONE;
			}
			if (Accept(Name.Mid(Found + Marker.Len())))
			{
				return Found;
			}
			SearchFrom = Found + 1;
		}
		return INDEX_NONE;
	}

	int32 GetConfiguredAutosaveSlotCount()
	{
		const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
		return Settings
			? FMath::Clamp(Settings->AutosaveSlotCount, 0, UEMSAutosaveSettings::MaxAutosaveSlots)
			: 0;
	}

	FDateTime GetSlotTime(UEMSObject* EMS, const FString& SlotName)
	{
		const UEMSInfoSaveGame* Info = EMS->GetSlotInfoObject(SlotName);
		return Info ? Info->SlotInfo.TimeStamp : FDateTime::MinValue();
	}
}

FString UEMSAutosaveSubsystem::MakeAutosaveSlotName(const FString& Profile, const int32 Index)
{
	return FSavePaths::ValidateSaveName(FString::Printf(TEXT("%s_Autosave_%d"), *Profile, Index));
}

FString UEMSAutosaveSubsystem::MakeManualSlotName(const FString& Profile, const FString& SaveName)
{
	return FSavePaths::ValidateSaveName(Profile + TEXT("_Manual_") + SaveName);
}

FString UEMSAutosaveSubsystem::MakeCheckpointSlotName(const FString& Profile, const FString& CheckpointSlotName)
{
	const FString Name = CheckpointSlotName.TrimStartAndEnd();
	return FSavePaths::ValidateSaveName(Name.IsEmpty()
		? Profile + TEXT("_Checkpoint")
		: Profile + TEXT("_Checkpoint_") + Name);
}

FString UEMSAutosaveSubsystem::GetSlotProfile(const FString& SlotName)
{
	const FString Name = FSavePaths::ValidateSaveName(SlotName);
	const FString Separator = StoredPart(TEXT("_"));

	const int32 Autosave = FindMarker(Name, StoredPart(TEXT("_Autosave_")), [](const FString& Rest)
	{
		return !Rest.IsEmpty()
			&& Rest.Len() <= 2
			&& Algo::AllOf(Rest, [](const TCHAR Char) { return FChar::IsDigit(Char); })
			&& FCString::Atoi(*Rest) < UEMSAutosaveSettings::MaxAutosaveSlots;
	});
	const int32 Manual = FindMarker(Name, StoredPart(TEXT("_Manual_")), [](const FString& Rest)
	{
		return !Rest.IsEmpty();
	});
	const int32 Checkpoint = FindMarker(Name, StoredPart(TEXT("_Checkpoint")), [&Separator](const FString& Rest)
	{
		return Rest.IsEmpty() || (Rest.StartsWith(Separator) && Rest.Len() > Separator.Len());
	});

	int32 ProfileEnd = INDEX_NONE;
	for (const int32 Found : { Autosave, Manual, Checkpoint })
	{
		if (Found != INDEX_NONE && (ProfileEnd == INDEX_NONE || Found < ProfileEnd))
		{
			ProfileEnd = Found;
		}
	}
	return ProfileEnd == INDEX_NONE ? Name : Name.Left(ProfileEnd);
}

FString UEMSAutosaveSubsystem::GetSaveProfile() const
{
	const UEMSObject* EMS = UEMSObject::Get(this);
	return EMS ? GetSlotProfile(EMS->GetCurrentSaveGameName()) : FString();
}

TArray<FString> UEMSAutosaveSubsystem::GetSaveSlots()
{
	TArray<FString> Result;
	UEMSObject* EMS = UEMSObject::Get(this);
	const FString Profile = GetSaveProfile();
	if (!EMS || Profile.IsEmpty())
	{
		return Result;
	}

	// EMS lists slots newest first.
	for (const FString& Slot : EMS->GetSortedSaveSlots())
	{
		if (GetSlotProfile(Slot).Equals(Profile, ESearchCase::IgnoreCase))
		{
			Result.Add(Slot);
		}
	}
	return Result;
}

void UEMSAutosaveSubsystem::AddCarriedCustomFile(const FString& FileName)
{
	const FString Trimmed = FileName.TrimStartAndEnd();
	if (!Trimmed.IsEmpty())
	{
		RuntimeCarriedCustomFiles.AddUnique(Trimmed);
	}
}

void UEMSAutosaveSubsystem::RemoveCarriedCustomFile(const FString& FileName)
{
	RuntimeCarriedCustomFiles.Remove(FileName.TrimStartAndEnd());
}

TArray<FString> UEMSAutosaveSubsystem::GetCarriedCustomFiles(const bool bIncludeCheckpointRecord) const
{
	const FString& RecordFile = GetDefault<UEMSCheckpointSaveGame>()->SaveGameName;
	TArray<FString> Files;
	if (bIncludeCheckpointRecord)
	{
		Files.Add(RecordFile);
	}
	// The checkpoint record is handled above only, whatever the project lists.
	const auto AddListed = [&Files, &RecordFile](const FString& File)
	{
		const FString Trimmed = File.TrimStartAndEnd();
		if (!Trimmed.IsEmpty() && !Trimmed.Equals(RecordFile, ESearchCase::IgnoreCase))
		{
			Files.AddUnique(Trimmed);
		}
	};
	if (const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>())
	{
		for (const FString& File : Settings->CarriedCustomFiles)
		{
			AddListed(File);
		}
	}
	for (const FString& File : RuntimeCarriedCustomFiles)
	{
		AddListed(File);
	}
	return Files;
}

FString UEMSAutosaveSubsystem::ResolveAutosaveTarget(const FString& Profile)
{
	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS || Profile.IsEmpty())
	{
		return FString();
	}

	const int32 Count = GetConfiguredAutosaveSlotCount();
	if (Count <= 0)
	{
		return FSavePaths::ValidateSaveName(Profile);
	}

	// The current slot is the progress being continued. With more than one autosave
	// slot it is never the one replaced, so a failed save can always switch back to it.
	const FString CurrentSlot = EMS->GetCurrentSaveGameName();
	FString Oldest;
	FDateTime OldestTime = FDateTime::MaxValue();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FString Name = MakeAutosaveSlotName(Profile, Index);
		if (Count > 1 && Name.Equals(CurrentSlot, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!EMS->DoesSaveSlotExist(Name))
		{
			return Name;
		}

		// Ties keep the lower index, so equal timestamps still rotate predictably.
		const FDateTime Time = GetSlotTime(EMS, Name);
		if (Oldest.IsEmpty() || Time < OldestTime)
		{
			Oldest = Name;
			OldestTime = Time;
		}
	}
	return Oldest;
}

bool UEMSAutosaveSubsystem::SwitchToSaveSlot(const FString& TargetSlot, const EEMSSlotSwitch Switch, bool* bOutRemovedLevelData)
{
	if (bOutRemovedLevelData)
	{
		*bOutRemovedLevelData = false;
	}

	UEMSObject* EMS = UEMSObject::Get(this);
	const FString ValidTarget = FSavePaths::ValidateSaveName(TargetSlot);
	if (!EMS || ValidTarget.IsEmpty())
	{
		return false;
	}
	if (EMS->GetCurrentSaveGameName().Equals(ValidTarget, ESearchCase::IgnoreCase))
	{
		return true;
	}

	const bool bStreamingSession = EMS->AutoSaveLoadWorldPartition() || EMS->AutoSaveLoadStreamingLevels();
	const bool bWorldPartitionRestoring = EMS->AlwaysAutoLoadWorldPartition();

#if EMS_VERSION_NUMBER >= 186
	/*
	EMS 1.86 keeps restoring streamed cells and sub-levels from memory only when the new slot has
	no Level data. Otherwise it clears memory loading like 1.85: World Partition reloads the whole
	level from the new slot's disk data when the next cell streams in, and streamed sub-levels come
	back unrestored. The save rewrites the Level file completely from session memory (EMS never
	merges the old file), so the stale one of the slot being replaced holds nothing the save needs.
	Removing it first lets the running session continue into the new slot.
	*/
	if (Switch == EEMSSlotSwitch::ContinueSession && bStreamingSession)
	{
		// A listed slot without Level data is no longer a loadable save. The caller deletes it
		// if the save does not start. Nothing happens when there is no Level file.
		if (bOutRemovedLevelData)
		{
			*bOutRemovedLevelData = EMS->DoesSaveSlotExist(ValidTarget);
		}
		// Refused only while an EMS task runs; then the switch below falls back to the 1.85 behavior.
		EMS->DeleteDataFromSlot(ValidTarget, EDeleteSlotType::Level);
	}
#endif

	EMS->SetCurrentSaveGameName(ValidTarget);
	const bool bSwitched = EMS->GetCurrentSaveGameName().Equals(ValidTarget, ESearchCase::IgnoreCase);

	if (bSwitched && bStreamingSession && Switch != EEMSSlotSwitch::LeaveWorld)
	{
		// World Partition reports whether memory restoration survived. Level streaming does not
		// expose it; there the switch keeps it exactly when the slot had no Level data.
		if (EMS->AutoSaveLoadWorldPartition())
		{
			if (bWorldPartitionRestoring && !EMS->AlwaysAutoLoadWorldPartition())
			{
				LogMessage(FString::Printf(
					TEXT("Switching to %s stopped EMS from restoring World Partition cells from memory. The next streamed cell reloads the level from that slot's saved data."),
					*ValidTarget), true);
			}
		}
#if EMS_VERSION_NUMBER >= 186
		else if (Switch == EEMSSlotSwitch::Return && EMS->DoesSaveSlotExist(ValidTarget))
#else
		else
#endif
		{
			LogMessage(FString::Printf(
				TEXT("Switching to %s stops EMS from restoring streamed levels from memory until the next load."),
				*ValidTarget), true);
		}
	}

	return bSwitched;
}

bool UEMSAutosaveSubsystem::ReturnFromAbandonedSave(const FString& PreviousSlot, const FString& AbandonedSlot, const bool bRemovedLevelData)
{
	return bRemovedLevelData
		? ReturnAndDiscardFailedSave(PreviousSlot, AbandonedSlot)
		: ReturnToPreviousSlot(PreviousSlot, AbandonedSlot);
}

bool UEMSAutosaveSubsystem::ReturnToPreviousSlot(const FString& PreviousSlot, const FString& FailedSlot, const EEMSSlotSwitch Switch)
{
	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS
		|| PreviousSlot.IsEmpty()
		|| !EMS->GetCurrentSaveGameName().Equals(FailedSlot, ESearchCase::IgnoreCase)
		|| PreviousSlot.Equals(FailedSlot, ESearchCase::IgnoreCase))
	{
		return true;
	}

	if (!SwitchToSaveSlot(PreviousSlot, Switch))
	{
		LogMessage(FString::Printf(TEXT("The save of %s failed and EMS did not switch back to %s."), *FailedSlot, *PreviousSlot), true);
		return false;
	}
	return true;
}

bool UEMSAutosaveSubsystem::ReturnAndDiscardFailedSave(const FString& PreviousSlot, const FString& FailedSlot)
{
	if (!ReturnToPreviousSlot(PreviousSlot, FailedSlot))
	{
		// Still on the failed slot; the journal lets the next start recover it.
		return false;
	}

	// EMS writes the slot info before any actor data, so a failed save leaves a slot
	// that Get Save Slots lists and Load Save accepts. An overwritten slot holds a mix
	// of its old save and the failed one. Neither may be offered to the player.
	// Only a slot the game has actually left is deleted: rewriting the current slot
	// in place has nothing earlier to fall back on.
	UEMSObject* EMS = UEMSObject::Get(this);
	const FString Failed = FSavePaths::ValidateSaveName(FailedSlot);
	if (!EMS
		|| Failed.IsEmpty()
		|| Failed.Equals(FSavePaths::ValidateSaveName(PreviousSlot), ESearchCase::IgnoreCase)
		|| EMS->GetCurrentSaveGameName().Equals(Failed, ESearchCase::IgnoreCase)
		|| !EMS->DoesSaveSlotExist(Failed))
	{
		return true;
	}

	// EMS refuses the delete while one of its tasks is running.
	EMS->DeleteAllSaveDataForSlot(Failed);
	if (EMS->DoesSaveSlotExist(Failed))
	{
		LogMessage(FString::Printf(TEXT("The save of %s failed and its incomplete data could not be deleted. Do not load it."), *Failed), true);
	}
	else
	{
		LogMessage(FString::Printf(TEXT("The save of %s failed; its incomplete data was deleted."), *Failed), true);
	}
	return true;
}

bool UEMSAutosaveSubsystem::CarryCustomFiles(const FString& SourceSlot, const FString& TargetSlot, const bool bIncludeCheckpointRecord)
{
	UGameInstance* GameInstance = GetGameInstance();
	UEMSObject* EMS = UEMSObject::Get(this);
	UEMSObjectAdv* EMSAdv = GameInstance ? GameInstance->GetSubsystem<UEMSObjectAdv>() : nullptr;
	const FString ValidSource = FSavePaths::ValidateSaveName(SourceSlot);
	const FString ValidTarget = FSavePaths::ValidateSaveName(TargetSlot);
	if (!EMS || !EMSAdv || ValidSource.IsEmpty() || ValidTarget.IsEmpty())
	{
		return false;
	}
	if (ValidSource.Equals(ValidTarget, ESearchCase::IgnoreCase))
	{
		return true;
	}

	// A 1.1 record means "this slot is the checkpoint". Name the slot explicitly
	// before the record is copied, or the copy would point at the target instead.
	if (bIncludeCheckpointRecord && !UpgradeLegacyCheckpointRecord(ValidSource))
	{
		return false;
	}

	TArray<FString> Present;
	for (const FString& File : GetCarriedCustomFiles(bIncludeCheckpointRecord))
	{
		if (EMS->DoesCustomSaveExist(ValidSource, File))
		{
			Present.Add(File);
			continue;
		}
		if (!EMS->DoesCustomSaveExist(ValidTarget, File))
		{
			continue;
		}

		// The source has no such file, so the target's copy is older data. Remove it
		// through EMS with a handle that names the file; nothing is read from disk.
		UEMSCheckpointSaveGame* Handle = NewObject<UEMSCheckpointSaveGame>(GetTransientPackage());
		Handle->SaveGameName = File;
		Handle->SlotName = ValidTarget;
		Handle->bUseSaveSlot = true;
		EMS->DeleteCustomSave(Handle);
		if (EMS->DoesCustomSaveExist(ValidTarget, File))
		{
			LogMessage(FString::Printf(TEXT("Could not remove the outdated custom file %s from %s."), *File, *ValidTarget), true);
			return false;
		}
	}

	return Present.IsEmpty()
		|| EMSAdv->CopySaveSlot(ValidTarget, ValidSource, ECopySaveSlotType::CustomFiles, Present, true);
}

bool UEMSAutosaveSubsystem::ManualSave(const FString& SaveName)
{
	static const FName ManualSaveReason(TEXT("Manual"));

	UWorld* World = GetWorld();
	UEMSObject* EMS = UEMSObject::Get(this);
	const FString Profile = GetSaveProfile();
	if (SaveName.TrimStartAndEnd().IsEmpty()
		|| Profile.IsEmpty()
		|| bIsShuttingDown
		|| bSaveActive
		|| ActiveSaveTask
		|| IsCheckpointLoadInProgress()
		|| IsSaveLoadInProgress()
		|| !World
		|| !World->IsGameWorld()
		|| World->bIsTearingDown
		|| !EMSAddons::HasPersistenceAuthority(World->GetNetMode())
		|| !EMS
		|| EMS->IsAsyncTaskActive(false)
		|| IsLevelStreamingBlockingSave())
	{
		return false;
	}

	StartSave(ManualSaveReason, Profile, MakeManualSlotName(Profile, SaveName.TrimStartAndEnd()), nullptr, nullptr);
	return bSaveActive;
}

bool UEMSAutosaveSubsystem::LoadSave(const FString& SlotName)
{
	UWorld* World = GetWorld();
	UEMSObject* EMS = UEMSObject::Get(this);
	if (bIsShuttingDown
		|| bSaveActive
		|| ActiveSaveTask
		|| ActiveLoadTask
		|| IsCheckpointLoadInProgress()
		|| IsSaveLoadInProgress()
		|| !World
		|| !World->IsGameWorld()
		|| World->bIsTearingDown
		|| !EMSAddons::HasPersistenceAuthority(World->GetNetMode())
		|| !EMS
		|| FAsyncSaveHelpers::IsAsyncTaskActive(ESaveGameMode::MODE_All, false))
	{
		return false;
	}

	const FString Slot = FSavePaths::ValidateSaveName(SlotName);
	if (Slot.IsEmpty() || !EMS->DoesSaveSlotExist(Slot))
	{
		LogMessage(FString::Printf(TEXT("Load Save refused %s: the slot does not exist."), *SlotName), true);
		return false;
	}

	const UEMSInfoSaveGame* Info = EMS->GetSlotInfoObject(Slot);
	const FName SavedLevel = Info ? Info->SlotInfo.Level : NAME_None;
	if (SavedLevel.IsNone())
	{
		LogMessage(FString::Printf(TEXT("Load Save refused %s: its slot info names no level."), *Slot), true);
		return false;
	}

	// A request queued before the load describes progress that is about to be replaced.
	if (PendingRequest.IsPending())
	{
		CancelPendingRequest(TEXT("Pending autosave canceled because a save load started."), false);
	}
	CheckpointAwaitingPlayerPlacement = FEMSCheckpointRecord();
	CheckpointLoadSlot.Reset();

	// The normal EMS switch: data comes from the slot's disk files. Clearing user data also
	// drops this session's multi-level and streaming memory, which belongs to the old progress.
	SlotBeforeTravelLoad = EMS->GetCurrentSaveGameName();
	EMS->SetCurrentSaveGameName(Slot);
	EMS->ClearUserData();
	if (!EMS->GetCurrentSaveGameName().Equals(Slot, ESearchCase::IgnoreCase))
	{
		LogMessage(FString::Printf(TEXT("Load Save could not make %s the current EMS slot."), *Slot), true);
		return false;
	}

	SaveBeingLoaded = Slot;
	SaveLoadLevel = SavedLevel;
	bSaveLoadAfterTravel = true;
	UGameplayStatics::OpenLevel(this, SavedLevel, true);
	return true;
}

void UEMSAutosaveSubsystem::BeginSaveLoad(UWorld* LoadedWorld)
{
	bSaveLoadAfterTravel = false;
	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	UEMSObject* EMS = UEMSObject::Get(this);
	if (!Settings || !EMS || !LoadedWorld || !EMSAddons::HasPersistenceAuthority(LoadedWorld->GetNetMode()))
	{
		FailSaveLoad(TEXT("The save could not be loaded because the new world is not valid for loading."));
		return;
	}
	if (!EMS->GetCurrentSaveGameName().Equals(SaveBeingLoaded, ESearchCase::IgnoreCase))
	{
		FailSaveLoad(TEXT("Save loading was canceled because the current EMS slot changed during travel."));
		return;
	}
	if (FName(*FLevelHelpers::GetWorldLevelName(LoadedWorld, false)) != SaveLoadLevel)
	{
		FailSaveLoad(FString::Printf(TEXT("Save travel ended in a different map than %s."), *SaveLoadLevel.ToString()));
		return;
	}

	// The game's Begin Play may already have started an EMS load of this slot.
	// Starting a second one would fail, so report that load's own result instead.
	if (FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_All, false))
	{
		if (!AdoptRunningLoad())
		{
			FailSaveLoad(TEXT("An EMS load was already running after travel, but it is not one load of the configured Save Data, so Load Save cannot report it."));
		}
		return;
	}

	ActiveLoadTask = UEMSAsyncLoadGame::AsyncLoadActors(this, Settings->SaveDataFlags, true);
	if (!ActiveLoadTask)
	{
		FailSaveLoad(TEXT("The save could not be loaded because EMS could not start a load operation."));
		return;
	}
	ActiveLoadTask->OnCompleted.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleLoadCompleted);
	ActiveLoadTask->OnFailed.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleLoadFailed);
	ActiveLoadTask->Activate();
}

bool UEMSAutosaveSubsystem::AdoptRunningLoad()
{
	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	if (!Settings)
	{
		return false;
	}

	// Exactly one running load, and it must load at least what Load Save promises.
	UEMSAsyncLoadGame* Running = nullptr;
	for (TObjectIterator<UEMSAsyncLoadGame> It; It; ++It)
	{
		if (IsValid(*It) && It->IsActive())
		{
			if (Running)
			{
				return false;
			}
			Running = *It;
		}
	}

	const ESaveGameMode Required = FAsyncSaveHelpers::GetMode(Settings->SaveDataFlags);
	if (!Running || (Running->Mode != ESaveGameMode::MODE_All && Running->Mode != Required))
	{
		return false;
	}

	AdoptedLoadTask = Running;
	Running->OnCompleted.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleLoadCompleted);
	Running->OnFailed.AddUniqueDynamic(this, &UEMSAutosaveSubsystem::HandleLoadFailed);
	LogMessage(TEXT("Load Save reports the EMS load the game started on arrival."));
	return true;
}

void UEMSAutosaveSubsystem::CompleteSaveLoad()
{
	const FString LoadedSlot = SaveBeingLoaded;
	ResetSaveLoadState();

	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS || !EMS->GetCurrentSaveGameName().Equals(LoadedSlot, ESearchCase::IgnoreCase))
	{
		LogMessage(TEXT("Save loading completed after the current EMS slot changed."), true);
		OnSaveLoadFailed.Broadcast(LoadedSlot);
		return;
	}

	LogMessage(FString::Printf(TEXT("Loaded save %s."), *LoadedSlot));
	RefreshCheckpointActorsFromSaveData();
	OnSaveLoaded.Broadcast(LoadedSlot);
}

void UEMSAutosaveSubsystem::FailSaveLoad(const FString& Message)
{
	const FString FailedSlot = SaveBeingLoaded;
	ResetSaveLoadState();
	LogMessage(Message, true);
	RefreshCheckpointActorsFromSaveData();
	if (!FailedSlot.IsEmpty())
	{
		OnSaveLoadFailed.Broadcast(FailedSlot);
	}
}

void UEMSAutosaveSubsystem::ResetSaveLoadState()
{
	if (UEMSAsyncLoadGame* Adopted = AdoptedLoadTask.Get())
	{
		Adopted->OnCompleted.RemoveDynamic(this, &UEMSAutosaveSubsystem::HandleLoadCompleted);
		Adopted->OnFailed.RemoveDynamic(this, &UEMSAutosaveSubsystem::HandleLoadFailed);
	}
	AdoptedLoadTask.Reset();
	SaveBeingLoaded.Reset();
	SaveLoadLevel = NAME_None;
	bSaveLoadAfterTravel = false;
	if (!bCheckpointLoadAfterTravel)
	{
		SlotBeforeTravelLoad.Reset();
	}
}

void UEMSAutosaveSubsystem::ReturnFromFailedTravel(const FString& LoadingSlot)
{
	UEMSObject* EMS = UEMSObject::Get(this);
	const FString Previous = SlotBeforeTravelLoad;
	SlotBeforeTravelLoad.Reset();
	if (!EMS
		|| Previous.IsEmpty()
		|| !EMS->GetCurrentSaveGameName().Equals(LoadingSlot, ESearchCase::IgnoreCase))
	{
		return;
	}

	// The old world keeps running, so its progress stays current. Its session memory was
	// already cleared for the load, so EMS reads that slot from disk again.
	EMS->SetCurrentSaveGameName(Previous);
	if (!EMS->GetCurrentSaveGameName().Equals(Previous, ESearchCase::IgnoreCase))
	{
		LogMessage(FString::Printf(TEXT("The load's travel failed and EMS did not switch back to %s."), *Previous), true);
	}
}

void UEMSAutosaveSubsystem::HandleTravelFailure(UWorld* World, ETravelFailure::Type FailureType, const FString& ErrorString)
{
	if (bIsShuttingDown || (World && World->GetGameInstance() != GetGameInstance()))
	{
		return;
	}

	// The map was not opened, so the load cannot start. Without this the load would wait
	// for its map forever and block every later save.
	if (bSaveLoadAfterTravel)
	{
		ReturnFromFailedTravel(SaveBeingLoaded);
		FailSaveLoad(FString::Printf(TEXT("Load Save could not open %s (%s): %s"),
			*SaveLoadLevel.ToString(), ETravelFailure::ToString(FailureType), *ErrorString));
	}
	else if (bCheckpointLoadAfterTravel)
	{
		ReturnFromFailedTravel(CheckpointLoadSlot);
		FailCheckpointLoad(FString::Printf(TEXT("Load Last Checkpoint could not open %s (%s): %s"),
			*CheckpointBeingLoaded.World.ToString(), ETravelFailure::ToString(FailureType), *ErrorString));
	}
}
