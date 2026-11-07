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
		|| IsCheckpointLoadInProgress()
		|| IsSaveLoadInProgress()
		|| CurrentSlot.IsEmpty())
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
		|| !InvalidateCheckpointStoredIn(TargetSlot, true)
		|| !CarryCustomFiles(CurrentSlot, TargetSlot))
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
	ActiveReason = Reason;
	ActiveSaveSlot = SaveSlot;
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
		OutTargetSlot = SaveSlot;
		return true;
	}
	return false;
}

void UEMSAutosaveSubsystem::CompleteSynchronousAutosave(
	const bool bSuccess,
	const int32 SaveDataFlags)
{
	if (!bSaveActive || ActiveSaveTask)
	{
		return;
	}

	const FName CompletedReason = ActiveReason;
	const FString CompletedSlot = ActiveSaveSlot;
	AddonSaveSlotAwaitingActorsSaved = CompletedSlot;

	if (bSuccess)
	{
		LastSuccessfulSaveSeconds = FPlatformTime::Seconds();
		OnAutosaveCompleted.Broadcast(CompletedReason);
	}
	else
	{
		// The failed slot may hold a partial save. Continue from the last complete one.
		ReturnAndDiscardFailedSave(ActivePreviousSlot, CompletedSlot);
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
		return;
	}

	if (UEMSObject* EMS = UEMSObject::Get(this); IsValid(EMS))
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
		return;
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
		return;
	}

	ResetActiveSaveState();
}

void UEMSAutosaveSubsystem::AbortSynchronousAutosave()
{
	if (!bSaveActive || ActiveSaveTask)
	{
		return;
	}

	const FName FailedReason = ActiveReason;
	const FString FailedSlot = ActiveSaveSlot;

	// Nothing was written, so the previous slot is still the newest complete save.
	ReturnToPreviousSlot(ActivePreviousSlot, FailedSlot, EEMSSlotSwitch::LeaveWorld);
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

	ResetActiveSaveState();
}
