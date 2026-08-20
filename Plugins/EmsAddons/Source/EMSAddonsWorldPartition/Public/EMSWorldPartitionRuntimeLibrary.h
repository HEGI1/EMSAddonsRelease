//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "EMSWorldPartitionRuntimeLibrary.generated.h"

UCLASS()
class EMSADDONSWORLDPARTITION_API UEMSWorldPartitionRuntimeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Spawns a managed runtime actor at the given transform.
	 *
	 * The spawn location must be covered by a currently visible supported generated
	 * cell, or the spawn is rejected and this returns null. This is not retried
	 * automatically: an unloaded cell at BeginPlay (e.g. spawning at the position of
	 * an always-loaded actor before streaming has caught up) will fail the spawn.
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "EMS Addons|World Partition Runtime Actors",
		meta = (WorldContext = "WorldContextObject", DeterminesOutputType = "ActorClass", DisplayName = "Spawn World Partition Runtime Actor", UnsafeDuringActorConstruction = "true"))
	static AActor* SpawnWorldPartitionRuntimeActor(
		const UObject* WorldContextObject,
		TSubclassOf<AActor> ActorClass,
		const FTransform& WorldTransform,
		ESpawnActorCollisionHandlingMethod CollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

	UFUNCTION(BlueprintPure, Category = "EMS Addons|World Partition Runtime Actors", meta = (WorldContext = "WorldContextObject"))
	static bool IsWorldPartitionRuntimeActorManaged(const UObject* WorldContextObject, const AActor* Actor);

	/**
	 * True while EMSAddons is removing a managed actor, either because its location
	 * streamed out or because a load is replacing the runtime records.
	 *
	 * Read this from End Play. A managed actor cannot otherwise tell that removal
	 * from gameplay destroying it, and the two usually deserve different reactions:
	 * an actor being taken out of the world should release what it owns without
	 * playing a death, dropping loot, or announcing itself as killed.
	 */
	UFUNCTION(
		BlueprintPure,
		Category = "EMS Addons|World Partition Runtime Actors",
		meta = (WorldContext = "WorldContextObject", DisplayName = "Is Managed Actor Removal In Progress"))
	static bool IsWorldPartitionRuntimeRemovalInProgress(const UObject* WorldContextObject);

	/**
	 * The currently live managed actors, in no particular order.
	 *
	 * Dormant records have no actor to return, so this is the live set and not the
	 * saved set. Callable rather than pure: it builds an array, and a pure node would
	 * rebuild it at every use site.
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "EMS Addons|World Partition Runtime Actors",
		meta = (WorldContext = "WorldContextObject", DisplayName = "Get World Partition Runtime Actors"))
	static TArray<AActor*> GetWorldPartitionRuntimeActors(const UObject* WorldContextObject);

	/** Diagnostic counts. Normal gameplay never needs to manage or reconcile cells manually. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|World Partition Runtime Actors|Diagnostics", meta = (WorldContext = "WorldContextObject"))
	static int32 GetManagedRuntimeActorCount(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "EMS Addons|World Partition Runtime Actors|Diagnostics", meta = (WorldContext = "WorldContextObject"))
	static int32 GetDormantRuntimeActorCount(const UObject* WorldContextObject);
};
