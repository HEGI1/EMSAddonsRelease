//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAutosaveSubsystem.h"

#include "EMSAsyncLoadGame.h"
#include "EMSAsyncSaveGame.h"
#include "EMSAsyncStream.h"
#include "EMSObject.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UObjectIterator.h"

/*
EMS advances its streaming tasks (streamed cell or sub-level loads, and the memory-only Level saves taken
when they unload) on world timers. Those stop while the game is paused, so a pause that lands in the few
frames such a task runs leaves it active until the game is unpaused, and EMS refuses every save and load in
the meantime. A pause menu's Save or Load would silently do nothing.

Manual Save, Load Save and a travelling Load Last Checkpoint therefore accept the request, unpause briefly,
let that streaming work finish, restore the pause and run. EMS's own paused save path is the precedent: it
unpauses for the duration of every save and pauses again afterwards. Only identifiable streaming loads
are waited out. EMS 1.85/1.86 keep the flags distinguishing automatic memory-only streaming saves from
ordinary disk saves private; every active save is therefore a conflict, including Level-only saves.
*/

namespace
{
	/** Streaming work takes a few frames. Anything longer is not what this wait is for. */
	constexpr double MaxPausedStreamingWaitSeconds = 2.0;
	const FName PausedManualSaveReason(TEXT("Manual"));
}

bool UEMSAutosaveSubsystem::IsStreamingStuckByPause() const
{
	const UWorld* World = GetWorld();
	if (!World || !World->IsPaused())
	{
		return false;
	}

	bool bAnyStreamingWork = false;
	for (TObjectIterator<UEMSAsyncLoadGame> It; It; ++It)
	{
		if (IsValid(*It) && It->IsActive())
		{
			return false;
		}
	}
	for (TObjectIterator<UEMSAsyncStream> It; It; ++It)
	{
		if (IsValid(*It) && It->IsActive())
		{
			bAnyStreamingWork = true;
		}
	}
	for (TObjectIterator<UEMSAsyncSaveGame> It; It; ++It)
	{
		if (IsValid(*It) && It->IsActive())
		{
			// MODE_Level also describes ordinary disk saves. EMS does not expose its
			// bAutoSaveLevel/bMemoryOnly flags, so mode cannot safely identify streaming.
			return false;
		}
	}
	return bAnyStreamingWork;
}

bool UEMSAutosaveSubsystem::WaitForStreamingStuckByPause(
	const EEMSPausedOperation Operation,
	const FString& Argument,
	const FText& DisplayName,
	const FEMSCheckpointRecord& Checkpoint)
{
	if (Operation == EEMSPausedOperation::None
		|| PausedOperation != EEMSPausedOperation::None
		|| bRunningPausedOperation
		|| bIsShuttingDown
		|| !IsStreamingStuckByPause())
	{
		return false;
	}

	UEMSObject* EMS = UEMSObject::Get(this);
	if (!EMS) return false;
	const FString User = EMS->GetCurrentSaveUserName();
	const FString Slot = EMS->GetCurrentSaveGameName();
	UGameplayStatics::SetGamePaused(this, false);
	if (UGameplayStatics::IsGamePaused(this))
	{
		// A pauser of the game refused to unpause; the streaming work cannot advance.
		LogMessage(TEXT("Save or load refused while paused: EMS streaming work is waiting for the game to unpause."), true);
		return false;
	}

	PausedOperation = Operation;
	PausedOperationArgument = Argument;
	PausedOperationUser = User;
	PausedOperationSlot = Slot;
	PausedOperationDisplayName = DisplayName;
	PausedOperationCheckpoint = Checkpoint;
	PausedOperationDeadline = FPlatformTime::Seconds() + MaxPausedStreamingWaitSeconds;
	// The core ticker keeps running whatever the world does, so the time limit always applies.
	PausedOperationTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UEMSAutosaveSubsystem::TickPausedOperation));
	LogMessage(TEXT("Paused during EMS streaming: the game runs until that streaming work finishes, then pauses again and the request runs."));
	return true;
}

bool UEMSAutosaveSubsystem::TickPausedOperation(float DeltaTime)
{
	if (PausedOperation == EEMSPausedOperation::None)
	{
		PausedOperationTicker.Reset();
		return false;
	}

	const UWorld* World = GetWorld();
	const UEMSObject* EMS = UEMSObject::Get(this);
	if (!World || World->bIsTearingDown || !EMS)
	{
		PausedOperationTicker.Reset();
		CancelPausedOperation(TEXT("The save or load requested while paused was canceled because its world ended."));
		return false;
	}

	if (EMS->IsAsyncTaskActive(false))
	{
		if (FPlatformTime::Seconds() < PausedOperationDeadline)
		{
			return true;
		}
		PausedOperationTicker.Reset();
		RunPausedOperation(false, TEXT("The save or load requested while paused failed: EMS streaming work did not finish in time."));
		return false;
	}

	PausedOperationTicker.Reset();
	RunPausedOperation(true, TEXT("The save or load requested while paused could not start once EMS streaming work had finished."));
	return false;
}

void UEMSAutosaveSubsystem::RunPausedOperation(const bool bReady, const TCHAR* FailureMessage)
{
	const EEMSPausedOperation Operation = PausedOperation;
	const FString Argument = PausedOperationArgument;
	const FString User = MoveTemp(PausedOperationUser);
	const FString Slot = MoveTemp(PausedOperationSlot);
	const FText DisplayName = PausedOperationDisplayName;
	const FEMSCheckpointRecord Checkpoint = PausedOperationCheckpoint;
	PausedOperation = EEMSPausedOperation::None;
	PausedOperationArgument.Reset();
	PausedOperationUser.Reset();
	PausedOperationSlot.Reset();
	PausedOperationDisplayName = FText();
	PausedOperationCheckpoint = FEMSCheckpointRecord();
	if (PausedOperationTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PausedOperationTicker);
		PausedOperationTicker.Reset();
	}

	// Hand the pause back first. A save then takes EMS's normal paused path: EMS unpauses for the save and
	// pauses again when it ends. A load travels, which ends the pause anyway.
	if (GetWorld() && !UGameplayStatics::IsGamePaused(this))
	{
		UGameplayStatics::SetGamePaused(this, true);
	}

	bool bStarted = false;
	const UEMSObject* EMS = UEMSObject::Get(this);
	bPausedOperationFailureReported = false;
	const bool bSameContext = EMS && EMS->GetCurrentSaveUserName() == User
		&& EMS->GetCurrentSaveGameName().Equals(Slot, ESearchCase::IgnoreCase);
	if (bReady && !bSameContext)
	{
		FailureMessage = TEXT("The save or load requested while paused was canceled because the EMS user or save slot changed.");
	}
	if (bReady && bSameContext)
	{
		TGuardValue<bool> RunningGuard(bRunningPausedOperation, true);
		switch (Operation)
		{
		case EEMSPausedOperation::ManualSave:
			bStarted = ManualSave(Argument, DisplayName);
			break;
		case EEMSPausedOperation::LoadSave:
			bStarted = LoadSave(Argument);
			break;
		case EEMSPausedOperation::LoadLastCheckpoint:
			bStarted = LoadLastCheckpoint();
			break;
		default:
			break;
		}
	}
	if (bStarted || bPausedOperationFailureReported)
	{
		return;
	}

	LogMessage(FailureMessage, true);
	switch (Operation)
	{
	case EEMSPausedOperation::ManualSave:
		OnAutosaveFailed.Broadcast(PausedManualSaveReason);
		break;
	case EEMSPausedOperation::LoadSave:
		OnSaveLoadFailed.Broadcast(Argument);
		break;
	case EEMSPausedOperation::LoadLastCheckpoint:
		if (Checkpoint.IsValid())
		{
			OnCheckpointLoadFailed.Broadcast(Checkpoint);
		}
		break;
	default:
		break;
	}
}

void UEMSAutosaveSubsystem::CancelPausedOperation(const TCHAR* Message)
{
	if (PausedOperation == EEMSPausedOperation::None)
	{
		return;
	}
	if (PausedOperationTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PausedOperationTicker);
		PausedOperationTicker.Reset();
	}

	const EEMSPausedOperation Operation = PausedOperation;
	const FString Argument = PausedOperationArgument;
	const FEMSCheckpointRecord Checkpoint = PausedOperationCheckpoint;
	PausedOperation = EEMSPausedOperation::None;
	PausedOperationArgument.Reset();
	PausedOperationUser.Reset();
	PausedOperationSlot.Reset();
	PausedOperationDisplayName = FText();
	PausedOperationCheckpoint = FEMSCheckpointRecord();
	LogMessage(Message, true);
	if (bIsShuttingDown)
	{
		return;
	}
	switch (Operation)
	{
	case EEMSPausedOperation::ManualSave:
		OnAutosaveFailed.Broadcast(PausedManualSaveReason);
		break;
	case EEMSPausedOperation::LoadSave:
		OnSaveLoadFailed.Broadcast(Argument);
		break;
	case EEMSPausedOperation::LoadLastCheckpoint:
		if (Checkpoint.IsValid())
		{
			OnCheckpointLoadFailed.Broadcast(Checkpoint);
		}
		break;
	default:
		break;
	}
}
