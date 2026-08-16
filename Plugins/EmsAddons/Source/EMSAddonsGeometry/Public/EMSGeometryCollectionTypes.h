//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "EMSGeometryCollectionTypes.generated.h"

UENUM(BlueprintType)
enum class EEMSGeometryAssetMismatchPolicy : uint8
{
	Reject,
	AllowIfHierarchyMatches
};

USTRUCT()
struct EMSADDONSGEOMETRY_API FEMSGeometryCollectionState
{
	GENERATED_BODY()

	static constexpr int32 CurrentVersion = 1;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Geometry")
	int32 Version = CurrentVersion;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Geometry")
	bool bHasValidData = false;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Geometry")
	FSoftObjectPath RestCollectionAsset;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Geometry")
	int64 HierarchyHash = 0;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Geometry")
	bool bWasRootBroken = false;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Geometry")
	TArray<FTransform> PieceTransforms;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Geometry")
	TArray<int32> BrokenIndices;

	/** Derived rather than saved, so the two can never disagree on load. */
	bool IsFractured() const
	{
		return bWasRootBroken || !BrokenIndices.IsEmpty();
	}

	void Reset()
	{
		*this = FEMSGeometryCollectionState();
	}
};
