//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "EMSWorldPartitionRuntimeTypes.generated.h"

USTRUCT(BlueprintType)
struct EMSADDONSWORLDPARTITION_API FEMSWorldPartitionRuntimeActorRecord
{
	GENERATED_BODY()

	static constexpr int32 CurrentVersion = 1;

	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|World Partition Runtime Actors")
	FGuid LocalId;

	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|World Partition Runtime Actors")
	FSoftClassPath ActorClass;

	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|World Partition Runtime Actors")
	FTransform SavedTransform = FTransform::Identity;

	UPROPERTY(SaveGame)
	TArray<uint8> ActorBinaryData;

	/** Supporting World Partition cell that must be visible before a dormant actor can return. */
	UPROPERTY(SaveGame)
	FGuid RequiredCellGuid;

	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|World Partition Runtime Actors")
	int32 RecordVersion = CurrentVersion;
};

/** Values used by the resolver's deterministic, order-independent ranking. */
struct EMSADDONSWORLDPARTITION_API FEMSWorldPartitionCellCandidateRank
{
	bool bLevelStreamingCell = false;
	bool bContainsLocation = false;
	bool bSpatiallyLoaded = false;
	bool bHLOD = false;
	bool bHasDataLayers = false;
	double XYArea = TNumericLimits<double>::Max();
	double Volume = TNumericLimits<double>::Max();
	FGuid CellGuid;
	FString DebugName;

	bool IsSupported() const
	{
		return bLevelStreamingCell && bContainsLocation && bSpatiallyLoaded && !bHLOD && !bHasDataLayers;
	}
};

namespace EMSAddonsWorldPartition
{
	/** Returns INDEX_NONE when there is no supported candidate. */
	EMSADDONSWORLDPARTITION_API int32 SelectBestCellCandidate(
		const TArray<FEMSWorldPartitionCellCandidateRank>& Candidates);
}
