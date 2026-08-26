//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAutosaveMapTravel.h"

#include "EMSAddonsAuthority.h"
#include "EMSAddonsAutosave.h"
#include "EMSAutosaveSettings.h"
#include "EMSAutosaveSubsystem.h"
#include "EMSMisc.h"
#include "EMSObject.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	const FName MapLeaveAutosaveReason(TEXT("MapLeave"));

	bool IsStreamingSaveBlocked(UEMSObject* EMS, const int32 SaveDataFlags)
	{
		if (!EMS || !EMSFLAG::IsLevel(SaveDataFlags) || !EMS->HasAnyLevelStreaming())
		{
			return false;
		}

		const bool bWorldPartitionBlocked =
			EMS->AutoSaveLoadWorldPartition()
			&& !EMS->WorldPartitionLoadComplete()
			&& !UEMSObject::SkipInitialWorldPartitionLoad();

		const bool bStreamingBlocked =
			!EMS->AutoSaveLoadWorldPartition()
			&& EMS->IsLevelStreaming();

		return bWorldPartitionBlocked || bStreamingBlocked;
	}

	void LogSkip(const FString& MapName, const TCHAR* Reason)
	{
		UE_LOG(
			LogEMSAddonsAutosave,
			Log,
			TEXT("Autosave When Leaving Map skipped before travelling to %s: %s"),
			*MapName,
			Reason);
	}
}

void EMSAutosaveMapTravel::HandlePreLoadMap(const FWorldContext& WorldContext, const FString& MapName)
{
	UWorld* World = WorldContext.World();
	if (!World
		|| !World->IsGameWorld()
		|| World->bIsTearingDown
		|| !EMSAddons::HasPersistenceAuthority(World->GetNetMode()))
	{
		return;
	}

	UGameInstance* GameInstance = World->GetGameInstance();
	if (!GameInstance)
	{
		return;
	}

	UEMSAutosaveSubsystem* Autosave = GameInstance->GetSubsystem<UEMSAutosaveSubsystem>();
	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	if (!Autosave || !Settings || !Settings->bEnableAutosave || !Settings->bAutosaveWhenLeavingMap)
	{
		return;
	}

	if (Autosave->IsCheckpointLoadInProgress())
	{
		LogSkip(MapName, TEXT("a checkpoint load is travelling to its restore map."));
		return;
	}

	if (Autosave->IsAutosaveBlocked())
	{
		LogSkip(MapName, TEXT("an autosave blocker is active."));
		return;
	}

	UEMSObject* EMS = GameInstance->GetSubsystem<UEMSObject>();
	if (!EMS || FAsyncSaveHelpers::ShouldCancelSaveTask(EMS))
	{
		LogSkip(MapName, TEXT("the outgoing EMS world is no longer valid for saving."));
		return;
	}

	const FString SaveSlot = EMS->GetCurrentSaveGameName();
	if (SaveSlot.IsEmpty())
	{
		LogSkip(MapName, TEXT("no current EMS save slot is set."));
		return;
	}

	if (Settings->SaveDataFlags <= 0)
	{
		LogSkip(MapName, TEXT("no EMS save data groups are enabled."));
		return;
	}

	if (Autosave->IsAutosaveActive() || EMS->IsAsyncTaskActive(false))
	{
		LogSkip(MapName, TEXT("another EMS save or load is still active."));
		return;
	}

	if (IsStreamingSaveBlocked(EMS, Settings->SaveDataFlags))
	{
		LogSkip(MapName, TEXT("level streaming is not ready for a complete save."));
		return;
	}

	Autosave->OnAutosaveStarted.Broadcast(MapLeaveAutosaveReason);

	// Mirror UEMSAsyncSaveGame's normal save path, but finish it synchronously
	// before LoadMap can begin tearing down the outgoing world.
	EMS->SaveSlotInfoObject(SaveSlot, true);
	EMS->PrepareLoadAndSaveActors(
		Settings->SaveDataFlags,
		EAsyncCheckType::CT_Save,
		EPrepareType::PT_Default);

	bool bSuccess = true;

	if (EMSFLAG::IsPlayer(Settings->SaveDataFlags))
	{
		bSuccess &= EMS->SavePlayerActors(EMS->GetPlayerController(), EMS->PlayerSaveFile());
	}

	if (EMSFLAG::IsLevel(Settings->SaveDataFlags))
	{
		const FLevelSnapshot Snapshot = EMS->CaptureActorSnapshot(false);
		const bool bLevelSaved = EMS->SaveLevelActors(false, Snapshot);
		if (!bLevelSaved && !Snapshot.StreamingActors.IsEmpty())
		{
			EMS->RestoreStreamingActors(Snapshot.StreamingActors);
		}
		bSuccess &= bLevelSaved;
	}

	if (bSuccess)
	{
		Autosave->OnAutosaveCompleted.Broadcast(MapLeaveAutosaveReason);
		UE_LOG(
			LogEMSAddonsAutosave,
			Log,
			TEXT("Autosave When Leaving Map saved %s before travelling to %s."),
			*EMS->GetLevelName().ToString(),
			*MapName);
	}
	else
	{
		Autosave->OnAutosaveFailed.Broadcast(MapLeaveAutosaveReason);
		UE_LOG(
			LogEMSAddonsAutosave,
			Warning,
			TEXT("Autosave When Leaving Map failed before travelling to %s."),
			*MapName);
	}

	// Keep checkpoint supersession, minimum-interval tracking and external EMS
	// listeners identical to a normal Save Game Actors operation.
	EMS->BroadcastOnActorsSaved(
		FAsyncSaveHelpers::GetMode(Settings->SaveDataFlags),
		bSuccess);
}
