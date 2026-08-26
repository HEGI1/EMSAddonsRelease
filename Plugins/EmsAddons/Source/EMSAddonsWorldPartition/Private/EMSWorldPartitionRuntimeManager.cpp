//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSWorldPartitionRuntimeManager.h"

#include "EMSAddonsAuthority.h"
#include "EMSAddonsGameThread.h"
#include "EMSWorldPartitionRuntimeSubsystem.h"
#include "EMSTypes.h"
#include "Engine/World.h"

namespace
{
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
	Tags.AddUnique(EMS::PersistentTag);
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
}

void AEMSWorldPartitionRuntimeManager::ActorLoaded_Implementation()
{
	if (UEMSWorldPartitionRuntimeSubsystem* Subsystem = ResolveRuntimeSubsystem(this))
	{
		Subsystem->HandleManagerLoaded(this);
	}
}
