//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAutosaveTypes.generated.h"

/** Persistent metadata identifying the latest checkpoint for an EMS save slot. */
USTRUCT(BlueprintType)
struct EMSADDONSAUTOSAVE_API FEMSCheckpointRecord
{
	GENERATED_BODY()

	static constexpr int32 CurrentVersion = 1;

	/** Serialized checkpoint-record version used to reject incompatible metadata. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Record")
	int32 Version = CurrentVersion;

	/** Persistent GUID of the checkpoint actor that produced this record. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Record")
	FGuid CheckpointId;

	/** Optional designer-facing name copied from the checkpoint actor. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Record")
	FText DisplayName;

	/** World asset containing the checkpoint. Used when checkpoint loading requires map travel. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Record")
	FSoftObjectPath World;

	/** UTC time at which the checkpoint activation was accepted. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Record")
	FDateTime ActivatedAt;

	/** Returns true when this record contains a supported version, checkpoint ID, and world. */
	bool IsValid() const
	{
		return Version == CurrentVersion && CheckpointId.IsValid() && !World.IsNull();
	}

	/** Returns true when both records identify the same checkpoint actor in the same world. */
	bool HasSameIdentity(const FEMSCheckpointRecord& Other) const
	{
		return CheckpointId.IsValid()
			&& Other.CheckpointId.IsValid()
			&& CheckpointId == Other.CheckpointId
			&& !World.IsNull()
			&& !Other.World.IsNull()
			&& World.GetAssetPath() == Other.World.GetAssetPath();
	}
};
