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

	// The synchronous operation must use one coherent configuration even if a
	// user callback mutates project settings while OnAutosaveStarted is running.
	const int32 SaveDataFlags = Settings->SaveDataFlags;

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

	if (SaveDataFlags <= 0)
	{
		LogSkip(MapName, TEXT("no EMS save data groups are enabled."));
		return;
	}

	if (Autosave->IsAutosaveActive() || EMS->IsAsyncTaskActive(false))
	{
		LogSkip(MapName, TEXT("another EMS save or load is still active."));
		return;
	}

	if (IsStreamingSaveBlocked(EMS, SaveDataFlags))
	{
		LogSkip(MapName, TEXT("level streaming is not ready for a complete save."));
		return;
	}

	if (!Autosave->BeginSynchronousAutosave(MapLeaveAutosaveReason, SaveSlot))
	{
		LogSkip(MapName, TEXT("the autosave subsystem could not reserve the map-leave save."));
		return;
	}

	// OnAutosaveStarted is user code. It may request another autosave, change the
	// save slot, add a blocker, start a direct EMS operation, or initiate travel.
	// Revalidate every assumption that must still hold before writing anything.
	const bool bStillValid =
		IsValid(World)
		&& World->IsGameWorld()
		&& !World->bIsTearingDown
		&& World->GetGameInstance() == GameInstance
		&& EMSAddons::HasPersistenceAuthority(World->GetNetMode())
		&& IsValid(EMS)
		&& !FAsyncSaveHelpers::ShouldCancelSaveTask(EMS)
		&& Autosave->IsAutosaveActive()
		&& !Autosave->IsAutosaveBlocked()
		&& EMS->GetCurrentSaveGameName().Equals(SaveSlot, ESearchCase::IgnoreCase)
		&& !EMS->IsAsyncTaskActive(false)
		&& !IsStreamingSaveBlocked(EMS, SaveDataFlags);
	if (!bStillValid)
	{
		Autosave->AbortSynchronousAutosave();
		LogSkip(MapName, TEXT("an autosave start callback invalidated the outgoing save operation."));
		return;
	}

	// Mirror UEMSAsyncSaveGame's normal save path, but finish it synchronously
	// before LoadMap can begin tearing down the outgoing world.
	EMS->SaveSlotInfoObject(SaveSlot, true);
	EMS->PrepareLoadAndSaveActors(
		SaveDataFlags,
		EAsyncCheckType::CT_Save,
		EPrepareType::PT_Default);

	bool bSuccess = true;

	if (EMSFLAG::IsPlayer(SaveDataFlags))
	{
		bSuccess &= EMS->SavePlayerActors(EMS->GetPlayerController(), EMS->PlayerSaveFile());
	}

	if (EMSFLAG::IsLevel(SaveDataFlags))
	{
		const FLevelSnapshot Snapshot = EMS->CaptureActorSnapshot(false);
		const bool bLevelSaved = EMS->SaveLevelActors(false, Snapshot);
		if (!bLevelSaved && !Snapshot.StreamingActors.IsEmpty())
		{
			EMS->RestoreStreamingActors(Snapshot.StreamingActors);
		}
		bSuccess &= bLevelSaved;
	}

	const FName SavedLevelName = EMS->GetLevelName();
	// Completion keeps the synchronous reservation through both the addon event
	// and EMS OnActorsSaved broadcast, so neither callback surface can start a
	// fresh async addon save immediately before LoadMap tears this world down.
	Autosave->CompleteSynchronousAutosave(bSuccess, SaveDataFlags);

	if (bSuccess)
	{
		UE_LOG(
			LogEMSAddonsAutosave,
			Log,
			TEXT("Autosave When Leaving Map saved %s before travelling to %s."),
			*SavedLevelName.ToString(),
			*MapName);
	}
	else
	{
		UE_LOG(
			LogEMSAddonsAutosave,
			Warning,
			TEXT("Autosave When Leaving Map failed before travelling to %s."),
			*MapName);
	}
}
