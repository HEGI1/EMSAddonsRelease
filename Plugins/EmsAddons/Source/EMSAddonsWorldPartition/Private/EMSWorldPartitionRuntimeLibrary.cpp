//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSWorldPartitionRuntimeLibrary.h"

#include "EMSWorldPartitionRuntimeSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace
{
	UEMSWorldPartitionRuntimeSubsystem* ResolveSubsystem(const UObject* WorldContextObject)
	{
		if (!GEngine || !WorldContextObject)
		{
			return nullptr;
		}
		UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
		return World ? World->GetSubsystem<UEMSWorldPartitionRuntimeSubsystem>() : nullptr;
	}
}

AActor* UEMSWorldPartitionRuntimeLibrary::SpawnWorldPartitionRuntimeActor(
	const UObject* WorldContextObject,
	TSubclassOf<AActor> ActorClass,
	const FTransform& WorldTransform,
	const ESpawnActorCollisionHandlingMethod CollisionHandlingOverride)
{
	UEMSWorldPartitionRuntimeSubsystem* Subsystem = ResolveSubsystem(WorldContextObject);
	return Subsystem ? Subsystem->SpawnManagedActor(ActorClass, WorldTransform, CollisionHandlingOverride) : nullptr;
}

bool UEMSWorldPartitionRuntimeLibrary::IsWorldPartitionRuntimeActorManaged(
	const UObject* WorldContextObject,
	const AActor* Actor)
{
	const UEMSWorldPartitionRuntimeSubsystem* Subsystem = ResolveSubsystem(WorldContextObject);
	return Subsystem && Subsystem->IsManagedActor(Actor);
}

bool UEMSWorldPartitionRuntimeLibrary::IsWorldPartitionRuntimeRemovalInProgress(const UObject* WorldContextObject)
{
	const UEMSWorldPartitionRuntimeSubsystem* Subsystem = ResolveSubsystem(WorldContextObject);
	return Subsystem && Subsystem->IsRemovingManagedActor();
}

TArray<AActor*> UEMSWorldPartitionRuntimeLibrary::GetWorldPartitionRuntimeActors(const UObject* WorldContextObject)
{
	TArray<AActor*> Actors;
	if (const UEMSWorldPartitionRuntimeSubsystem* Subsystem = ResolveSubsystem(WorldContextObject))
	{
		Subsystem->GetManagedActors(Actors);
	}
	return Actors;
}

int32 UEMSWorldPartitionRuntimeLibrary::GetManagedRuntimeActorCount(const UObject* WorldContextObject)
{
	const UEMSWorldPartitionRuntimeSubsystem* Subsystem = ResolveSubsystem(WorldContextObject);
	return Subsystem ? Subsystem->GetManagedActorCount() : 0;
}

int32 UEMSWorldPartitionRuntimeLibrary::GetDormantRuntimeActorCount(const UObject* WorldContextObject)
{
	const UEMSWorldPartitionRuntimeSubsystem* Subsystem = ResolveSubsystem(WorldContextObject);
	return Subsystem ? Subsystem->GetDormantActorCount() : 0;
}
