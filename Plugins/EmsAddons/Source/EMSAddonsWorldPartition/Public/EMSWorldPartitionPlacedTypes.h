//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSWorldPartitionRuntimeTypes.h"
#include "EMSWorldPartitionPlacedTypes.generated.h"

/** Separate from runtime records: absence is a new placement, not destruction. */
USTRUCT()
struct EMSADDONSWORLDPARTITION_API FEMSWorldPartitionPlacedActorRecord
{
	GENERATED_BODY()

	/** LocalId is the component's editor-authored PlacementId. */
	UPROPERTY(SaveGame)
	FEMSWorldPartitionRuntimeActorRecord State;

	/** Preserve the original persistent-level path when recreating the actor. */
	UPROPERTY(SaveGame)
	FName ActorName;

	UPROPERTY(SaveGame)
	FName ComponentName;

	/** Instance markers must be recreated; native/SCS markers come from the class. */
	UPROPERTY(SaveGame)
	bool bInstanceComponent = false;

	/** A tombstone must survive map reloads even though there is no live actor. */
	UPROPERTY(SaveGame)
	bool bDestroyed = false;
};
