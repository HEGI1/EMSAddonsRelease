//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAutosaveSubsystem.h"

#include "Algo/AllOf.h"
#include "EMSAddonsAuthority.h"
#include "EMSAsyncLoadGame.h"
#include "EMSAutosaveSettings.h"
#include "EMSCheckpointSaveGame.h"
#include "EMSCheckpointUtils.h"
#include "EMSInfoSaveGame.h"
#include "EMSLevel.h"
#include "EMSMisc.h"
#include "EMSObject.h"
#include "EMSObjectAdv.h"
#include "EMSPaths.h"
#include "EMSVersion.h"
#include "EMSSaveSlotMetadata.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"

/*
Save slots are ordinary EMS slots of one profile:

  <Profile>                          autosave with Autosave Slot Count 0 (1.1 behavior)
  <profile>_autosave_<0..N-1>        rotating autosaves, oldest replaced first
  <profile>_manual_<name>            Manual Save
  <profile>_checkpoint[_<name>]      checkpoint saves, named on the checkpoint actor

A save switches the current EMS slot to its target and runs a normal EMS save there.
EMS keeps its multi-level memory and its streaming cache across any switch, so the new slot
receives the complete session. EMS only keeps restoring streamed World Partition cells and
sub-levels from that cache when the new slot has no Level data; switching to a slot with Level
data stops memory restoration until that slot's Level file is loaded or saved. A save therefore
removes the stale Level data of the slot it replaces before switching to it. Loading switches
the slot the normal way, which makes EMS read the slot from disk.

EMS backups (Get Backup Name: "<slot>_bak0", "bak0_<slot>" on consoles) are read-only. A backup
can be loaded and belongs to the profile of its primary slot, but no addon save ever writes into
one, and no name is accepted that EMS would resolve to one. A save that continues a loaded backup
into the slot it backs up first restores the backup onto that slot, so EMS's auto backup of the
slot's previous files never replaces the loaded backup.
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

	/**
	 * False when EMS stores nothing of Name, such as "??": it removes characters a file name cannot hold.
	 * A slot ID ending in its marker is not recognized as that profile's save (see Get Slot Profile).
	 */
	bool HasStoredNamePart(const FString& Name)
	{
		return FSavePaths::ValidateSaveName(FString(TEXT("A")) + Name.TrimStartAndEnd()).Len() > 1;
	}

	/** True when EMS reserves the slot ID, for example for a backup. Such a slot is never an addon save target. */
	bool IsReservedSlot(const FString& SlotName)
	{
		return FSavePaths::IsSlotNameReserved(FSavePaths::ValidateSaveName(SlotName));
	}

	FString ResolveExistingSlot(const TArray<FString>& Slots, const FString& SlotName)
	{
		if (Slots.Contains(SlotName))
		{
			return SlotName;
		}
		for (const FString& Slot : Slots)
		{
			if (Slot.Equals(SlotName, ESearchCase::IgnoreCase))
			{
				return Slot;
			}
		}
		return SlotName;
	}
}

FString UEMSAutosaveSubsystem::MakeAutosaveSlotName(const FString& Profile, const int32 Index)
{
	return FSavePaths::ValidateSaveName(FString::Printf(TEXT("%s_autosave_%d"), *Profile, Index)).ToLower();
}

FString UEMSAutosaveSubsystem::MakeManualSlotName(const FString& Profile, const FString& SaveName)
{
	return FSavePaths::ValidateSaveName(Profile + TEXT("_manual_") + SaveName).ToLower();
}

FString UEMSAutosaveSubsystem::MakeCheckpointSlotName(const FString& Profile, const FString& CheckpointSlotName)
{
	// A name EMS stores nothing of selects the default checkpoint slot, like an empty one.
	// So does a name that would make the slot an EMS backup ("..._bak0"), which is read-only.
	const FString Name = CheckpointSlotName.TrimStartAndEnd();
	const FString DefaultSlot = FSavePaths::ValidateSaveName(Profile + TEXT("_checkpoint")).ToLower();
	if (!HasStoredNamePart(Name))
	{
		return DefaultSlot;
	}
	const FString NamedSlot = FSavePaths::ValidateSaveName(Profile + TEXT("_checkpoint_") + Name).ToLower();
	return IsReservedSlot(NamedSlot) ? DefaultSlot : NamedSlot;
}

FString UEMSAutosaveSubsystem::GetPrimarySlotName(const FString& SlotName)
{
	const FString Name = FSavePaths::ValidateSaveName(SlotName);
	FString Primary;
	return FSavePaths::GetBackupBaseName(Name, Primary) ? Primary : Name;
}

bool UEMSAutosaveSubsystem::IsBackupSaveUserSelected(const UEMSObject* EMS)
{
	// Mirrors EMS: a Save User whose folder is a backup is read-only for every write.
	const FString User = EMS ? EMS->GetCurrentSaveUserName() : FString();
	return !User.IsEmpty() && FSavePaths::IsBackupSavePath(FSavePaths::ValidateSaveName(User));
}

bool UEMSAutosaveSubsystem::CanWriteAddonSave(const FString& TargetSlot, const TCHAR* Operation) const
{
	const UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS)
	{
		return false;
	}
	// Refused before anything is written: preparing a save removes the target's stale Level data and
	// deletes a failed target, which in a backup would destroy the very data the backup protects.
	if (IsBackupSaveUserSelected(EMS))
	{
		LogMessage(FString::Printf(TEXT("%s refused: the current EMS Save User %s is a read-only backup."),
			Operation, *EMS->GetCurrentSaveUserName()), true);
		return false;
	}
	if (TargetSlot.TrimStartAndEnd().IsEmpty() || IsReservedSlot(TargetSlot))
	{
		LogMessage(FString::Printf(TEXT("%s refused: EMS reserves the slot name %s, for example for a read-only backup."),
			Operation, *FSavePaths::ValidateSaveName(TargetSlot)), true);
		return false;
	}
	return true;
}

FString UEMSAutosaveSubsystem::ResolveSaveSlotTarget(const FString& SlotName) const
{
	// Preserve existing spelling on case-sensitive save systems. New IDs are
	// lowercase, but saves made by earlier versions must keep their paths.
	const UEMSObject* EMS = UEMSObject::Get(this);
	return EMS ? ResolveExistingSlot(EMS->GetSortedSaveSlots(), SlotName) : SlotName;
}

FString UEMSAutosaveSubsystem::GetSlotProfile(const FString& SlotName)
{
	// A backup belongs to the profile of the slot it backs up, so a game continued from a
	// loaded backup keeps saving into that profile instead of starting a new one.
	const FString Name = GetPrimarySlotName(SlotName);
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
		if (GetSlotProfile(Slot).Equals(Profile, ESearchCase::IgnoreCase) && !IsSlotUnavailable(Slot))
		{
			Result.Add(Slot);
		}
	}
	return Result;
}

TArray<FEMSSaveSlotDetails> UEMSAutosaveSubsystem::GetSaveSlotsWithDetails()
{
	TArray<FEMSSaveSlotDetails> Result;
	for (const FString& Slot : GetSaveSlots())
	{
		FEMSSaveSlotDetails Details;
		if (GetSaveSlotDetails(Slot, Details))
		{
			Result.Add(MoveTemp(Details));
		}
	}
	return Result;
}

bool UEMSAutosaveSubsystem::GetSaveSlotDetails(const FString& SlotName, FEMSSaveSlotDetails& Details)
{
	Details = FEMSSaveSlotDetails();
	UEMSObject* EMS = UEMSObject::Get(this);
	const FString Slot = FSavePaths::ValidateSaveName(SlotName);
	if (!EMS || SlotName.TrimStartAndEnd().IsEmpty() || Slot.IsEmpty() || IsSlotUnavailable(Slot) || !EMS->DoesSaveSlotExist(Slot))
	{
		return false;
	}

	Details.SlotName = Slot;
	Details.TimeStamp = GetSlotTime(EMS, Slot);
	// A backup is named like the slot it backs up.
	const FString Named = GetPrimarySlotName(Slot);
	const FString Suffix = Named.Mid(GetSlotProfile(Named).Len());
	const FString ManualMarker = StoredPart(TEXT("_Manual_"));
	const FString CheckpointMarker = StoredPart(TEXT("_Checkpoint"));
	const FString AutosaveMarker = StoredPart(TEXT("_Autosave_"));
	if (Suffix.StartsWith(ManualMarker))
	{
		Details.SaveType = EEMSSaveSlotType::Manual;
		Details.DisplayName = FText::FromString(Suffix.Mid(ManualMarker.Len()));
	}
	else if (Suffix.StartsWith(CheckpointMarker))
	{
		Details.SaveType = EEMSSaveSlotType::Checkpoint;
		const FString NamedMarker = CheckpointMarker + StoredPart(TEXT("_"));
		Details.DisplayName = Suffix.StartsWith(NamedMarker)
			? FText::FromString(Suffix.Mid(NamedMarker.Len()))
			: NSLOCTEXT("EMSAutosave", "CheckpointName", "Checkpoint");
	}
	else
	{
		Details.DisplayName = Suffix.StartsWith(AutosaveMarker)
			? FText::Format(NSLOCTEXT("EMSAutosave", "AutosaveNumber", "Autosave {0}"),
				FText::AsNumber(FCString::Atoi(*Suffix.Mid(AutosaveMarker.Len())) + 1))
			: NSLOCTEXT("EMSAutosave", "AutosaveName", "Autosave");
	}

	const FString& File = GetDefault<UEMSSaveSlotMetadata>()->SaveGameName;
	if (EMS->DoesCustomSaveExist(Slot, File))
	{
		if (const UEMSSaveSlotMetadata* Metadata = Cast<UEMSSaveSlotMetadata>(
			EMS->GetCustomSave(UEMSSaveSlotMetadata::StaticClass(), Slot, FString())))
		{
			Details.SaveType = Metadata->SaveType;
			if (!Metadata->DisplayName.IsEmptyOrWhitespace())
			{
				Details.DisplayName = Metadata->DisplayName;
			}
		}
	}
	else
	{
		// An older checkpoint already has a label. Only use a record naming this slot;
		// later autosaves carry that record but are not themselves the checkpoint.
		const FEMSCheckpointRecord Record = GetCheckpointForSlot(Slot);
		if (Record.IsValid() && FSavePaths::ValidateSaveName(Record.SaveSlot).Equals(Named, ESearchCase::IgnoreCase))
		{
			Details.SaveType = EEMSSaveSlotType::Checkpoint;
			if (!Record.DisplayName.IsEmptyOrWhitespace())
			{
				Details.DisplayName = Record.DisplayName;
			}
		}
	}
	return true;
}

bool UEMSAutosaveSubsystem::WriteSaveSlotMetadata(
	const FString& SlotName, const FText& DisplayName, const EEMSSaveSlotType SaveType,
	const bool bMarkJournaledSaveComplete)
{
	UEMSObject* EMS = UEMSObject::Get(this);
	UEMSSaveSlotMetadata* Metadata = EMS ? Cast<UEMSSaveSlotMetadata>(
		EMS->GetCustomSave(UEMSSaveSlotMetadata::StaticClass(), SlotName, FString())) : nullptr;
	UEMSInfoSaveGame* Info = EMS ? EMS->GetSlotInfoObject(SlotName) : nullptr;
	if (!Metadata || !Info)
	{
		return false;
	}
	const FText PreviousName = Metadata->DisplayName;
	const EEMSSaveSlotType PreviousType = Metadata->SaveType;
	const FString PreviousSaveId = Metadata->CompletedSaveId;
	Metadata->DisplayName = DisplayName.IsEmptyOrWhitespace() ? FText() : DisplayName;
	Metadata->SaveType = SaveType;
	if (bMarkJournaledSaveComplete
		&& !InterruptedSaveId.IsEmpty()
		&& InterruptedSaveSlot.Equals(FSavePaths::ValidateSaveName(SlotName), ESearchCase::IgnoreCase))
	{
		// This write is the last one of the save. Recovery reads it as proof of completion,
		// so a journal that could not be cleared afterwards never deletes this slot.
		Metadata->CompletedSaveId = InterruptedSaveId;
	}
	// SaveCustom normally stamps Slot Info too. A label edit must not move an old
	// save to the front of the menu or change which autosave rotation replaces.
	// Hold EMS's redundant-write throttle for this metadata-only operation.
	TGuardValue<double> SlotInfoThrottle(Info->LastSaveAttemptTime, DBL_MAX);
	if (!EMS->SaveCustom(Metadata))
	{
		Metadata->DisplayName = PreviousName;
		Metadata->SaveType = PreviousType;
		Metadata->CompletedSaveId = PreviousSaveId;
		LogMessage(TEXT("The save slot's display metadata could not be written."), true);
		return false;
	}
	return true;
}

bool UEMSAutosaveSubsystem::SetSaveSlotDisplayName(const FString& SlotName, FText DisplayName)
{
	UEMSObject* EMS = UEMSObject::Get(this);
	FEMSSaveSlotDetails Details;
	if (bIsShuttingDown || bSaveActive || ActiveSaveTask || PausedOperation != EEMSPausedOperation::None
		|| IsCheckpointLoadInProgress() || IsSaveLoadInProgress()
		|| !EMS || EMS->IsAsyncTaskActive(false)
		|| !GetWorld() || !EMSAddons::HasPersistenceAuthority(GetWorld()->GetNetMode())
		|| !GetSaveSlotDetails(SlotName, Details)
		|| !CanWriteAddonSave(Details.SlotName, TEXT("Set Save Slot Display Name")))
	{
		return false;
	}
	return WriteSaveSlotMetadata(Details.SlotName, DisplayName, Details.SaveType);
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
	const FString& MetadataFile = GetDefault<UEMSSaveSlotMetadata>()->SaveGameName;
	TArray<FString> Files;
	if (bIncludeCheckpointRecord)
	{
		Files.Add(RecordFile);
	}
	// The checkpoint record is handled above only, whatever the project lists.
	const auto AddListed = [&Files, &RecordFile, &MetadataFile](const FString& File)
	{
		const FString Trimmed = File.TrimStartAndEnd();
		if (!Trimmed.IsEmpty() && !Trimmed.Equals(RecordFile, ESearchCase::IgnoreCase)
			&& !Trimmed.Equals(MetadataFile, ESearchCase::IgnoreCase))
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
		// The profile comes from the current slot, which is lowercase after any
		// generated save. Keep writing the caller's original profile slot. From a loaded
		// backup of it, the save restores that backup first (RestoreOwnBackupBeforeSave).
		return ResolveSaveSlotTarget(FSavePaths::ValidateSaveName(Profile));
	}

	// The current slot is the progress being continued. With more than one autosave
	// slot it is never the one replaced, so a failed save can always switch back to it.
	// A loaded backup counts as its primary slot: writing that primary would also replace
	// the backup with the primary's older files (EMS auto backup) while it is still in use.
	const FString CurrentSlot = GetPrimarySlotName(EMS->GetCurrentSaveGameName());
	const TArray<FString> ExistingSlots = EMS->GetSortedSaveSlots();
	FString Oldest;
	FString Incomplete;
	FDateTime OldestTime = FDateTime::MaxValue();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FString Name = ResolveExistingSlot(ExistingSlots, MakeAutosaveSlotName(Profile, Index));
		if (Count > 1 && Name.Equals(CurrentSlot, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!EMS->DoesSaveSlotExist(Name))
		{
			return Name;
		}
		// A slot known to be incomplete holds no save to lose, whatever its time says: a save that
		// was cut off or canceled may have stamped it last. Replace it before any complete save.
		if (IsSlotUnavailable(Name))
		{
			if (Incomplete.IsEmpty())
			{
				Incomplete = Name;
			}
			continue;
		}

		// Ties keep the lower index, so equal timestamps still rotate predictably.
		const FDateTime Time = GetSlotTime(EMS, Name);
		if (Oldest.IsEmpty() || Time < OldestTime)
		{
			Oldest = Name;
			OldestTime = Time;
		}
	}
	return Incomplete.IsEmpty() ? Oldest : Incomplete;
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

	// A save continues the session into its target. A read-only backup never is one, and the
	// Level data removal below would delete the backup's own files.
	if (Switch == EEMSSlotSwitch::ContinueSession && IsReservedSlot(ValidTarget))
	{
		LogMessage(FString::Printf(TEXT("Save refused: EMS reserves the slot name %s, for example for a read-only backup."), *ValidTarget), true);
		return false;
	}

	const bool bStreamingSession = EMS->AutoSaveLoadWorldPartition() || EMS->AutoSaveLoadStreamingLevels();
	const bool bWorldPartitionRestoring = EMS->AlwaysAutoLoadWorldPartition();
	const FFileContext LevelFile(0, EMS->ActorSaveFile(ValidTarget));

	/*
	EMS keeps restoring streamed cells and sub-levels from its streaming cache only when the new slot
	has no Level data. Otherwise memory restoration stops until that slot's Level file is loaded or
	saved: World Partition reloads the whole level from the file when the next cell streams in, which
	brings the replaced save's older actor state back into the running world, and streamed sub-levels
	come back unrestored. The save rewrites the Level file completely from session memory (EMS never
	merges the old file), so the stale one of the slot being replaced holds nothing the save needs.
	Removing it first lets the running session continue into the new slot.
	This applies to saves without Level data too. Keeping the file would make streamed cells reload
	that older Level data into the running world. The slot's Player data is replaced anyway, so its
	old Level data no longer matches it; the game's next Level save writes the file again.
	*/
	if (Switch == EEMSSlotSwitch::ContinueSession && bStreamingSession)
	{
		const bool bHadLevelData = FSaveFileIntegrity::DoesSaveExist(LevelFile);
		EMS->DeleteDataFromSlot(ValidTarget, EDeleteSlotType::Level);
		if (FSaveFileIntegrity::DoesSaveExist(LevelFile))
		{
			LogMessage(FString::Printf(TEXT("Save refused: the old Level data of %s could not be removed; switching would stop streaming restoration."), *ValidTarget), true);
			return false;
		}
		// Removing Level data invalidates the old snapshot. The caller deletes its slot
		// if the save does not start. Nothing was removed when there was no Level file.
		if (bOutRemovedLevelData)
		{
			*bOutRemovedLevelData = bHadLevelData && EMS->DoesSaveSlotExist(ValidTarget);
		}
	}

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
		else if (Switch == EEMSSlotSwitch::Return && FSaveFileIntegrity::DoesSaveExist(LevelFile))
		{
			LogMessage(FString::Printf(
				TEXT("Switching to %s stops EMS from restoring streamed levels from memory until that slot is loaded or saved again."),
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
	// A start callback can switch EMS users before the task is activated. Never roll
	// back a same-named slot belonging to that other user.
	if (EMS && bSaveActive && !EMS->GetCurrentSaveUserName().Equals(ActiveSaveUser, ESearchCase::IgnoreCase))
	{
		return true;
	}
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
	const UEMSObject* CurrentEMS = UEMSObject::Get(this);
	if (!CurrentEMS || (bSaveActive && !CurrentEMS->GetCurrentSaveUserName().Equals(ActiveSaveUser, ESearchCase::IgnoreCase)))
	{
		return true;
	}
	// Failure callbacks can run before the active state is reset; they must already see the exclusion.
	// A stale start that a callback replaced with other work must not end that work's own target.
	if (FailedSlot.Equals(ActiveSaveSlot, ESearchCase::IgnoreCase))
	{
		bSaveTargetInFlight = false;
	}
	SetSlotUnavailable(FailedSlot, true);
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
		|| !DoesSaveSlotRemain(Failed))
	{
		return true;
	}

	// EMS refuses the delete while one of its tasks is running.
	EMS->DeleteAllSaveDataForSlot(Failed);
	if (DoesSaveSlotRemain(Failed))
	{
		LogMessage(FString::Printf(TEXT("The save of %s failed and its incomplete data could not be deleted. Do not load it."), *Failed), true);
	}
	else
	{
		SetSlotUnavailable(Failed, false);
		LogMessage(FString::Printf(TEXT("The save of %s failed; its incomplete data was deleted."), *Failed), true);
	}
	return true;
}

bool UEMSAutosaveSubsystem::HasWrittenActiveSlotInfo() const
{
	UEMSObject* EMS = UEMSObject::Get(this);
	const UEMSInfoSaveGame* Info = EMS ? EMS->GetSlotInfoObject(ActiveSaveSlot) : nullptr;
	// EMS does not propagate this write's result to the actor-save task. It updates this
	// clock only after a successful write and restores the old info on failure.
	if (!Info || Info->LastSaveAttemptTime < ActiveSlotInfoWriteStartSeconds)
	{
		LogMessage(TEXT("The actor save finished, but EMS did not write its slot info. The save is incomplete."), true);
		return false;
	}
	return true;
}

bool UEMSAutosaveSubsystem::DoesSaveSlotRemain(const FString& SlotName) const
{
	const UEMSObject* EMS = UEMSObject::Get(this);
	// Desktop EMS also lists directories whose Slot Info was removed but whose
	// remaining custom files could not be deleted. Slot Info alone cannot prove cleanup.
	return EMS && (EMS->DoesSaveSlotExist(SlotName)
		|| EMS->GetSortedSaveSlots().ContainsByPredicate([&SlotName](const FString& Slot)
		{
			return Slot.Equals(SlotName, ESearchCase::IgnoreCase);
		}));
}

bool UEMSAutosaveSubsystem::IsSlotUnavailable(const FString& SlotName) const
{
	const UEMSObject* EMS = UEMSObject::Get(this);
	const FString Slot = FSavePaths::ValidateSaveName(SlotName);
	// The exclusion is persisted before a save prepares its target, to cover that save ending
	// badly. While the save still runs, its target is the current progress: hiding it would
	// drop the only slot of a single-slot profile from menus and report no current checkpoint
	// for the length of every save. Each way a save ends without completing leaves the
	// exclusion in place, and it applies again as soon as the save has failed or ended.
	if (EMS && bSaveTargetInFlight && !ActiveSaveSlot.IsEmpty()
		&& Slot.Equals(FSavePaths::ValidateSaveName(ActiveSaveSlot), ESearchCase::IgnoreCase)
		&& EMS->GetCurrentSaveUserName().Equals(ActiveSaveUser, ESearchCase::IgnoreCase))
	{
		return false;
	}
	return EMS && UnavailableSlots.ContainsByPredicate([&](const FEMSUnavailableSaveSlot& Entry)
	{
		return Entry.User.Equals(EMS->GetCurrentSaveUserName(), ESearchCase::IgnoreCase)
			&& Entry.Slot.Equals(Slot, ESearchCase::IgnoreCase);
	});
}

void UEMSAutosaveSubsystem::SetSlotUnavailable(const FString& SlotName, const bool bUnavailable)
{
	const UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS || SlotName.IsEmpty())
	{
		return;
	}
	const FString Slot = FSavePaths::ValidateSaveName(SlotName).ToLower();
	const FString User = EMS->GetCurrentSaveUserName().ToLower();
	const auto Matches = [&](const FEMSUnavailableSaveSlot& Entry)
	{
		return Entry.User.Equals(User, ESearchCase::IgnoreCase) && Entry.Slot.Equals(Slot, ESearchCase::IgnoreCase);
	};
	if (bUnavailable)
	{
		if (UnavailableSlots.ContainsByPredicate(Matches)) return;
		FEMSUnavailableSaveSlot& Entry = UnavailableSlots.AddDefaulted_GetRef();
		Entry.User = User;
		Entry.Slot = Slot;
	}
	else
	{
		if (UnavailableSlots.RemoveAll(Matches) == 0) return;
	}
	PersistSaveState();
}

bool UEMSAutosaveSubsystem::PreflightCarriedCustomFiles(const FString& SourceSlot, const FString& TargetSlot,
	const bool bIncludeCheckpointRecord) const
{
	const UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS)
	{
		return false;
	}
	if (SourceSlot.Equals(TargetSlot, ESearchCase::IgnoreCase))
	{
		return true;
	}
	for (const FString& File : GetCarriedCustomFiles(bIncludeCheckpointRecord))
	{
		if (!EMS->DoesCustomSaveExist(SourceSlot, File))
		{
			continue;
		}
		const FString Path = EMS->CustomSaveFile(File, SourceSlot);
		TArray<uint8> Data;
		if (FSaveFileIntegrity::LoadFromFile(FFileContext(0, Path), Data) != EFileValidity::FILE_VALID
			|| !FSaveFileIntegrity::VerifyLocalFile(Path, Data))
		{
			LogMessage(FString::Printf(TEXT("Could not read carried custom file %s from %s."), *File, *SourceSlot), true);
			return false;
		}
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
	// An EMS backup is read-only, so its record is upgraded in the target after the copy.
	// It names the backup's primary slot: EMS replaces the backup's files whenever the primary
	// is written again, so only the primary can keep (or invalidate) that checkpoint.
	const FString SourcePrimary = GetPrimarySlotName(ValidSource);
	const bool bBackupSource = !SourcePrimary.Equals(ValidSource, ESearchCase::IgnoreCase);
	bool bUpgradeInTarget = false;
	bool bDropRecord = false;
	if (bIncludeCheckpointRecord)
	{
		if (!bBackupSource && !UpgradeLegacyCheckpointRecord(ValidSource))
		{
			return false;
		}
		// A record naming the target names the save this one replaces, so that checkpoint
		// ends here. A continued backup holds its primary's own record, for example.
		UEMSCheckpointSaveGame* SourceSave = ResolveCheckpointSave(ValidSource);
		FEMSCheckpointRecord SourceRecord;
		bDropRecord = EMSCheckpoint::ResolveRecord(SourceSave, SourcePrimary, SourceRecord)
			&& FSavePaths::ValidateSaveName(SourceRecord.SaveSlot).Equals(ValidTarget, ESearchCase::IgnoreCase);
		bUpgradeInTarget = bBackupSource && !bDropRecord && EMSCheckpoint::IsLoadableLegacyRecord(SourceSave);
	}
	const FString& RecordFile = GetDefault<UEMSCheckpointSaveGame>()->SaveGameName;

	TArray<FString> Present;
	for (const FString& File : GetCarriedCustomFiles(bIncludeCheckpointRecord))
	{
		const bool bDropped = bDropRecord && File.Equals(RecordFile, ESearchCase::IgnoreCase);
		if (!bDropped && EMS->DoesCustomSaveExist(ValidSource, File))
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

	if (!Present.IsEmpty()
		&& !EMSAdv->CopySaveSlot(ValidTarget, ValidSource, ECopySaveSlotType::CustomFiles, Present, true))
	{
		return false;
	}
	return !bUpgradeInTarget || UpgradeLegacyCheckpointRecord(ValidTarget, SourcePrimary);
}

bool UEMSAutosaveSubsystem::RestoreOwnBackupBeforeSave(const FString& CurrentSlot, const FString& TargetSlot)
{
	UGameInstance* GameInstance = GetGameInstance();
	UEMSObject* EMS = UEMSObject::Get(this);
	UEMSObjectAdv* EMSAdv = GameInstance ? GameInstance->GetSubsystem<UEMSObjectAdv>() : nullptr;
	const FString ValidCurrent = FSavePaths::ValidateSaveName(CurrentSlot);
	const FString ValidTarget = FSavePaths::ValidateSaveName(TargetSlot);
	if (ValidCurrent.IsEmpty()
		|| ValidTarget.IsEmpty()
		|| ValidCurrent.Equals(ValidTarget, ESearchCase::IgnoreCase)
		|| !GetPrimarySlotName(ValidCurrent).Equals(ValidTarget, ESearchCase::IgnoreCase))
	{
		return true;
	}
	if (!EMS || !EMSAdv)
	{
		return false;
	}

	/*
	The game continues a loaded EMS backup into the slot it backs up (Autosave Slot Count 0 or 1, or a
	manual or checkpoint save of the same name). With "Auto Backup Previous Data", every file EMS writes
	into that slot first copies the slot's previous file over the backup's. Without this restore, the
	save would replace the loaded backup with the slot's previous save, file by file, and a failed save
	would return the game to that mix. EMS's own restore (Copy Save Slot from a backup onto its slot) keeps the backup
	unchanged, so afterwards those copies write the backup's own data back. The slot's newer save is
	replaced, as the player chose to continue the backup.
	*/
	if (!EMSAdv->CopySaveSlot(ValidTarget, ValidCurrent, ECopySaveSlotType::FullSlot, GetCarriedCustomFiles(true), true))
	{
		LogMessage(FString::Printf(TEXT("The save was not started: EMS could not restore the backup %s onto %s before saving."), *ValidCurrent, *ValidTarget), true);
		return false;
	}

	// A restore keeps the primary files the backup has no copy of. Writing them would back them up
	// into the backup, so remove them now; the save writes its own.
	for (const EDeleteSlotType Type : { EDeleteSlotType::Player, EDeleteSlotType::Level })
	{
		const bool bPlayer = Type == EDeleteSlotType::Player;
		const FFileContext TargetFile(0, bPlayer ? EMS->PlayerSaveFile(ValidTarget) : EMS->ActorSaveFile(ValidTarget));
		const FFileContext BackupFile(0, bPlayer ? EMS->PlayerSaveFile(ValidCurrent) : EMS->ActorSaveFile(ValidCurrent));
		if (FSaveFileIntegrity::DoesSaveExist(BackupFile) || !FSaveFileIntegrity::DoesSaveExist(TargetFile))
		{
			continue;
		}
		EMS->DeleteDataFromSlot(ValidTarget, Type);
		if (FSaveFileIntegrity::DoesSaveExist(TargetFile))
		{
			LogMessage(FString::Printf(TEXT("The save was not started: %s keeps data that its backup %s does not have."), *ValidTarget, *ValidCurrent), true);
			return false;
		}
	}

	LogMessage(FString::Printf(TEXT("Restored the loaded backup %s onto %s before saving; the newer save in %s was replaced."), *ValidCurrent, *ValidTarget, *ValidTarget), false);
	return true;
}

bool UEMSAutosaveSubsystem::ManualSave(const FString& SaveName, FText DisplayName)
{
	static const FName ManualSaveReason(TEXT("Manual"));

	UWorld* World = GetWorld();
	UEMSObject* EMS = UEMSObject::Get(this);
	const FString Profile = GetSaveProfile();
	if (!SaveName.TrimStartAndEnd().IsEmpty() && !HasStoredNamePart(SaveName))
	{
		// "<profile>_manual_" would be a slot outside the profile, which then becomes the current one.
		LogMessage(FString::Printf(TEXT("Manual Save refused: EMS cannot store any character of the save name \"%s\"."), *SaveName), true);
		return false;
	}
	if (SaveName.TrimStartAndEnd().IsEmpty()
		|| Profile.IsEmpty()
		|| bIsShuttingDown
		|| bSaveActive
		|| ActiveSaveTask
		|| IsCheckpointLoadInProgress()
		|| IsSaveLoadInProgress()
		|| PausedOperation != EEMSPausedOperation::None
		|| !World
		|| !World->IsGameWorld()
		|| World->bIsTearingDown
		|| !EMSAddons::HasPersistenceAuthority(World->GetNetMode())
		|| !EMS
		|| IsLevelStreamingBlockingSave()
		|| IsSavingExcludedOn(World, TEXT("Manual Save"))
		// A name such as "Old_bak0" would make the slot the read-only backup of another save.
		|| !CanWriteAddonSave(MakeManualSlotName(Profile, SaveName.TrimStartAndEnd()), TEXT("Manual Save")))
	{
		return false;
	}
	if (EMS->IsAsyncTaskActive(false))
	{
		// A pause that stopped EMS streaming mid-task: accepted, and saved once that work has finished.
		return WaitForStreamingStuckByPause(EEMSPausedOperation::ManualSave, SaveName, DisplayName);
	}

	StartSave(ManualSaveReason, Profile, ResolveSaveSlotTarget(MakeManualSlotName(Profile, SaveName.TrimStartAndEnd())), nullptr, nullptr,
		DisplayName.IsEmptyOrWhitespace() ? FText::FromString(SaveName.TrimStartAndEnd()) : DisplayName,
		EEMSSaveSlotType::Manual);
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
		|| PausedOperation != EEMSPausedOperation::None
		|| !World
		|| !World->IsGameWorld()
		|| World->bIsTearingDown
		|| !EMSAddons::HasPersistenceAuthority(World->GetNetMode())
		|| !EMS)
	{
		return false;
	}

	const FString Slot = FSavePaths::ValidateSaveName(SlotName);
	if (Slot.IsEmpty() || IsSlotUnavailable(Slot) || !EMS->DoesSaveSlotExist(Slot))
	{
		LogMessage(FString::Printf(TEXT("Load Save refused %s: the slot does not exist or is known to be incomplete."), *SlotName), true);
		return false;
	}

	const UEMSInfoSaveGame* Info = EMS->GetSlotInfoObject(Slot);
	const FName SavedLevel = Info ? Info->SlotInfo.Level : NAME_None;
	if (SavedLevel.IsNone())
	{
		LogMessage(FString::Printf(TEXT("Load Save refused %s: its slot info names no level."), *Slot), true);
		return false;
	}
	if (FAsyncSaveHelpers::IsAsyncTaskActive(ESaveGameMode::MODE_All, false))
	{
		// A pause that stopped EMS streaming mid-task: accepted, and loaded once that work has finished.
		return WaitForStreamingStuckByPause(EEMSPausedOperation::LoadSave, SlotName);
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
		bPausedOperationFailureReported |= bRunningPausedOperation;
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
		bPausedOperationFailureReported |= bRunningPausedOperation;
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
