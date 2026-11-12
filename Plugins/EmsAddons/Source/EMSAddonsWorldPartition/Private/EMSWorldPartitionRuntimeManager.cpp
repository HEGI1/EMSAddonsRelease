//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSWorldPartitionRuntimeManager.h"

#include "EMSAddonsAuthority.h"
#include "EMSAddonsGameThread.h"
#include "EMSAddonsWorldPartition.h"
#include "EMSPaths.h"
#include "EMSWorldPartitionRuntimeSubsystem.h"
#include "EMSWorldPartitionPlacedSubsystem.h"
#include "EMSTypes.h"
#include "Engine/World.h"

namespace
{
	FName GetWorldPackage(const UWorld* World)
	{
		return World ? FName(*FRedirectHelpers::ResolveWorldLevel(
			UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()))) : NAME_None;
	}

	//The authority rule needs the world anyway, and reading it here is what keeps the
	//three interface events from dereferencing a null world to reach the subsystem.
	UEMSWorldPartitionRuntimeSubsystem* ResolveRuntimeSubsystem(AEMSWorldPartitionRuntimeManager* Manager)
	{
		UWorld* World = EMSAddons::HasPersistenceAuthority(Manager) ? Manager->GetWorld() : nullptr;
		return World ? World->GetSubsystem<UEMSWorldPartitionRuntimeSubsystem>() : nullptr;
	}
}

AEMSWorldPartitionRuntimeManager::AEMSWorldPartitionRuntimeManager()
{
	PrimaryActorTick.bCanEverTick = false;
	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
	SetCanBeDamaged(false);
	SetReplicates(false);
#if WITH_EDITOR
	//Editor-only: the flag lives in the World Partition actor descriptor a placed manager
	//is cooked into. A manager the subsystem spawns at runtime goes straight into the
	//persistent level, which World Partition never streams, so it needs no equivalent.
	SetIsSpatiallyLoaded(false);
#endif
	//Persistent-level ownership keeps this actor resident while cells stream. EMS's
	//Persistent tag means slot-wide, cross-map state instead, which this registry is not.
}

void AEMSWorldPartitionRuntimeManager::Serialize(FArchive& Ar)
{
	const bool bLoadingSave = Ar.IsLoading() && Ar.IsSaveGame();
	if (bLoadingSave)
	{
		//Missing fields must not inherit ownership or records from an earlier load.
		SavedWorldPackage = NAME_None;
		RuntimeActorRecords.Reset();
		PlacedActorRecords.Reset();
		bRejectedSaveData = false;
	}
	Super::Serialize(Ar);
	if (bLoadingSave)
	{
		const FName CurrentWorldPackage = GetWorldPackage(GetWorld());
		const FName ResolvedSavedPackage(*FRedirectHelpers::ResolveWorldLevel(SavedWorldPackage.ToString()));
		if (SavedWorldPackage.IsNone() || CurrentWorldPackage.IsNone() || ResolvedSavedPackage != CurrentWorldPackage)
		{
			RuntimeActorRecords.Reset();
			PlacedActorRecords.Reset();
			bRejectedSaveData = true;
			UE_LOG(LogEMSAddonsWorldPartition, Warning,
				TEXT("Ignored World Partition manager save data with missing or different map ownership. SavedMap=%s CurrentMap=%s. Legacy unscoped records cannot be migrated safely; runtime actors reset and placed actors use authored defaults."),
				*SavedWorldPackage.ToString(), *CurrentWorldPackage.ToString());
		}
	}
}

void AEMSWorldPartitionRuntimeManager::ActorPreSave_Implementation()
{
	//Capture touches actors, components, and the World Partition, and the subsystem
	//refuses to operate off the game thread. EMS only marshals Pre-Save itself when
	//its Pre-Save On Game Thread setting is on, and that setting defaults to off, so
	//without this a Multi-Thread save would silently keep the previous binaries.
	const TWeakObjectPtr<AEMSWorldPartitionRuntimeManager> WeakThis(this);
	EMSAddons::RunOnGameThread(
		[WeakThis]()
		{
			AEMSWorldPartitionRuntimeManager* Manager = WeakThis.Get();
			if (!IsValid(Manager))
			{
				return;
			}

			if (UEMSWorldPartitionRuntimeSubsystem* Subsystem = ResolveRuntimeSubsystem(Manager))
			{
				Subsystem->HandleManagerPreSave(Manager);
			}
			if (UEMSWorldPartitionPlacedSubsystem* Placed = EMSAddons::HasPersistenceAuthority(Manager)
				? Manager->GetWorld()->GetSubsystem<UEMSWorldPartitionPlacedSubsystem>() : nullptr)
			{
				Placed->HandleManagerPreSave(Manager);
			}
			if (UEMSWorldPartitionRuntimeSubsystem* Subsystem = ResolveRuntimeSubsystem(Manager))
			{
				Subsystem->FinalizeManagerCapture(Manager);
			}
			if (IsValid(Manager)) Manager->SavedWorldPackage = GetWorldPackage(Manager->GetWorld());
		});
}

//The load events need no marshalling. EMS dispatches both of them directly, on
//whichever thread the load runs, and that is always the game thread.
void AEMSWorldPartitionRuntimeManager::ActorPreLoad_Implementation()
{
	if (UEMSWorldPartitionRuntimeSubsystem* Subsystem = ResolveRuntimeSubsystem(this))
	{
		Subsystem->HandleManagerPreLoad(this);
	}
	if (EMSAddons::HasPersistenceAuthority(this))
	{
		if (UEMSWorldPartitionPlacedSubsystem* Placed = GetWorld()->GetSubsystem<UEMSWorldPartitionPlacedSubsystem>()) Placed->HandleManagerPreLoad(this);
	}
}

void AEMSWorldPartitionRuntimeManager::ActorLoaded_Implementation()
{
	if (UEMSWorldPartitionRuntimeSubsystem* Subsystem = ResolveRuntimeSubsystem(this))
	{
		Subsystem->HandleManagerLoaded(this);
	}
	if (EMSAddons::HasPersistenceAuthority(this))
	{
		if (UEMSWorldPartitionPlacedSubsystem* Placed = GetWorld()->GetSubsystem<UEMSWorldPartitionPlacedSubsystem>()) Placed->HandleManagerLoaded(this);
	}
	if (bRejectedSaveData)
	{
		//An old global record and a new map record can coexist in a legacy stack.
		//Do not let the rejected record prevent EMS from applying the valid one,
		//regardless of their order in the archive or loader batch.
		Tags.Remove(EMS::HasLoadedTag);
		bRejectedSaveData = false;
	}
}
