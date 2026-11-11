//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAutosaveSubsystem.h"

#include "EMSMisc.h"
#include "EMSObject.h"
#include "HAL/PlatformTime.h"

bool UEMSAutosaveSubsystem::BeginSynchronousAutosave(
	const FName Reason,
	const FString& CurrentSlot,
	FString& OutTargetSlot)
{
	OutTargetSlot.Reset();
	if (bIsShuttingDown
		|| bSaveActive
		|| ActiveSaveTask
		|| PausedOperation != EEMSPausedOperation::None
		|| IsCheckpointLoadInProgress()
		|| IsSaveLoadInProgress()
		|| CurrentSlot.IsEmpty()
		|| IsSavingExcludedOn(GetWorld(), nullptr))
	{
		return false;
	}

	// A checkpoint request carries activation state that a map-leave convenience
	// save must never supersede. An ordinary pending save is older than the
	// outgoing-map snapshot and can safely be replaced by it.
	if (PendingRequest.IsCheckpoint())
	{
		return false;
	}
	if (PendingRequest.IsPending())
	{
		CancelPendingRequest(
			TEXT("Pending autosave was superseded by the synchronous map-leave save."),
			false);
	}

	// Same target, carried files and slot switch as an asynchronous autosave.
	const FString TargetSlot = ResolveAutosaveTarget(GetSlotProfile(CurrentSlot));
	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS
		|| TargetSlot.IsEmpty()
		|| !EMS->GetCurrentSaveGameName().Equals(CurrentSlot, ESearchCase::IgnoreCase)
		|| !PreflightCarriedCustomFiles(CurrentSlot, TargetSlot, true))
	{
		return false;
	}
	// Carrying custom files is already a write to the snapshot, even if the map-leave
	// callback later cancels before EMS writes actors. Protect it across restarts too.
	SetSlotUnavailable(TargetSlot, true);
	if (!InvalidateCheckpointStoredIn(TargetSlot, true))
	{
		return false;
	}
	if (!CarryCustomFiles(CurrentSlot, TargetSlot))
	{
		return false;
	}
	BeginSaveJournal(TargetSlot, CurrentSlot);
	// The outgoing world ends right after this save, so its streaming memory does not matter
	// and the target keeps its old save until EMS overwrites it.
	if (!SwitchToSaveSlot(TargetSlot, EEMSSlotSwitch::LeaveWorld))
	{
		ClearSaveJournal();
		return false;
	}
	const FString SaveSlot = EMS->GetCurrentSaveGameName();

	// Reserve before exposing the start callback. RequestAutosave may be called by
	// a listener, but TryStartPendingSave then sees bSaveActive and only queues it.
	bSaveActive = true;
	bSaveTargetInFlight = true;
	bNextActorsSavedIsFailedAddonSave = false;
	ActiveReason = Reason;
	ActiveDisplayName = AutosaveDisplayName;
	ActiveSaveType = EEMSSaveSlotType::Autosave;
	ActiveSaveSlot = SaveSlot;
	ActiveSaveUser = EMS->GetCurrentSaveUserName();
	ActiveSlotInfoWriteStartSeconds = FPlatformTime::Seconds();
	ActivePreviousSlot = CurrentSlot;
	bActiveRequestIsCheckpoint = false;
	ActiveCheckpoint = FEMSCheckpointRecord();
	ActiveCheckpointActor.Reset();

	OnAutosaveStarted.Broadcast(ActiveReason);

	// World cleanup (which also returns to the previous slot) or another callback
	// may have invalidated the reservation.
	if (bSaveActive
		&& ActiveSaveTask == nullptr
		&& ActiveReason == Reason
		&& ActiveSaveSlot.Equals(SaveSlot, ESearchCase::IgnoreCase))
	{
		if (!EMS->GetCurrentSaveUserName().Equals(ActiveSaveUser, ESearchCase::IgnoreCase))
		{
			AbortSynchronousAutosave();
			return false;
		}
		OutTargetSlot = SaveSlot;
		return true;
	}
	return false;
}

bool UEMSAutosaveSubsystem::CompleteSynchronousAutosave(
	bool bSuccess,
	const int32 SaveDataFlags)
{
	if (!bSaveActive || ActiveSaveTask)
	{
		return false;
	}

	const FName CompletedReason = ActiveReason;
	const FString CompletedSlot = ActiveSaveSlot;
	AddonSaveSlotAwaitingActorsSaved = CompletedSlot;
	const UEMSObject* CurrentEMS = UEMSObject::Get(this);
	bSuccess = bSuccess && CurrentEMS
		&& CurrentEMS->GetCurrentSaveUserName().Equals(ActiveSaveUser, ESearchCase::IgnoreCase)
		&& CurrentEMS->GetCurrentSaveGameName().Equals(CompletedSlot, ESearchCase::IgnoreCase)
		&& HasWrittenActiveSlotInfo();
	if (bSuccess)
	{
		bSuccess = WriteSaveSlotMetadata(CompletedSlot, ActiveDisplayName, ActiveSaveType);
	}
	const bool bClearJournal = bSuccess || ReturnAndDiscardFailedSave(ActivePreviousSlot, CompletedSlot);

	if (bSuccess)
	{
		SetSlotUnavailable(CompletedSlot, false);
		LastSuccessfulSaveSeconds = FPlatformTime::Seconds();
		OnAutosaveCompleted.Broadcast(CompletedReason);
	}
	else
	{
		// The failed slot may hold a partial save. Continue from the last complete one.
		OnAutosaveFailed.Broadcast(CompletedReason);
	}

	// Keep ownership across addon completion callbacks too. A listener may request
	// another addon save, but it must remain pending until this pre-load-map
	// operation and the equivalent EMS completion broadcast are both finished.
	if (!bSaveActive
		|| ActiveSaveTask
		|| ActiveReason != CompletedReason
		|| !ActiveSaveSlot.Equals(CompletedSlot, ESearchCase::IgnoreCase))
	{
		// The reservation is gone, so the EMS broadcast below is skipped.
		AddonSaveSlotAwaitingActorsSaved.Reset();
		return bSuccess;
	}

	if (UEMSObject* EMS = UEMSObject::Get(this); IsValid(EMS)
		&& EMS->GetCurrentSaveUserName().Equals(ActiveSaveUser, ESearchCase::IgnoreCase))
	{
		EMS->BroadcastOnActorsSaved(
			FAsyncSaveHelpers::GetMode(SaveDataFlags),
			bSuccess);
	}

	// EMS listeners are user code too. World cleanup may already have cleared the
	// reservation, in which case it also owns cleanup of any pending request.
	if (!bSaveActive
		|| ActiveSaveTask
		|| ActiveReason != CompletedReason
		|| !ActiveSaveSlot.Equals(CompletedSlot, ESearchCase::IgnoreCase))
	{
		return bSuccess;
	}

	// Any addon request raised during start/completion callbacks is too late for
	// the outgoing world and must not become a fresh async save just before
	// LoadMap tears that world down. A checkpoint cancellation is reported once;
	// if its failure callback queues more work, discard that nested request
	// silently to avoid recursive callback chains at the travel boundary.
	if (PendingRequest.IsPending())
	{
		CancelPendingRequest(
			TEXT("Autosave request raised during the synchronous map-leave save was superseded by that save."),
			PendingRequest.IsCheckpoint());
	}
	if (PendingRequest.IsPending())
	{
		CancelPendingRequest(
			TEXT("Nested autosave request raised while completing the synchronous map-leave save was canceled."),
			false);
	}

	if (!bSaveActive
		|| ActiveSaveTask
		|| ActiveReason != CompletedReason
		|| !ActiveSaveSlot.Equals(CompletedSlot, ESearchCase::IgnoreCase))
	{
		return bSuccess;
	}

	ResetActiveSaveState(bClearJournal);
	return bSuccess;
}

void UEMSAutosaveSubsystem::AbortSynchronousAutosave()
{
	if (!bSaveActive || ActiveSaveTask)
	{
		return;
	}

	const FName FailedReason = ActiveReason;
	const FString FailedSlot = ActiveSaveSlot;
	// The reservation is kept through the failure callbacks below, which must already see the exclusion.
	bSaveTargetInFlight = false;

	// Actor data was not written, but carried custom files may have changed. The target
	// stays persistently excluded while the previous complete slot becomes current again.
	const bool bReturned = ReturnToPreviousSlot(ActivePreviousSlot, FailedSlot, EEMSSlotSwitch::LeaveWorld);
	if (!FailedReason.IsNone())
	{
		OnAutosaveFailed.Broadcast(FailedReason);
	}

	if (!bSaveActive
		|| ActiveSaveTask
		|| ActiveReason != FailedReason
		|| !ActiveSaveSlot.Equals(FailedSlot, ESearchCase::IgnoreCase))
	{
		return;
	}

	if (PendingRequest.IsPending())
	{
		CancelPendingRequest(
			TEXT("Autosave request raised during an aborted synchronous map-leave save was canceled with it."),
			PendingRequest.IsCheckpoint());
	}
	if (PendingRequest.IsPending())
	{
		CancelPendingRequest(
			TEXT("Nested autosave request raised while aborting the synchronous map-leave save was canceled."),
			false);
	}

	if (!bSaveActive
		|| ActiveSaveTask
		|| ActiveReason != FailedReason
		|| !ActiveSaveSlot.Equals(FailedSlot, ESearchCase::IgnoreCase))
	{
		return;
	}

	// Still on the target when EMS refused the switch back; the journal lets the next start return.
	ResetActiveSaveState(bReturned);
}
