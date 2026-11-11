//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAutosaveTypes.generated.h"

/** A target that may contain an incomplete snapshot, scoped to its EMS user. */
USTRUCT()
struct FEMSUnavailableSaveSlot
{
	GENERATED_BODY()

	UPROPERTY()
	FString User;

	UPROPERTY()
	FString Slot;
};

UENUM(BlueprintType)
enum class EEMSSaveSlotType : uint8
{
	Autosave,
	Manual,
	Checkpoint
};

/** Menu data for a save. Display Name is presentation only; use Slot Name to load or overwrite. */
USTRUCT(BlueprintType)
struct EMSADDONSAUTOSAVE_API FEMSSaveSlotDetails
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Saves")
	FString SlotName;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Saves")
	FText DisplayName;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Saves")
	EEMSSaveSlotType SaveType = EEMSSaveSlotType::Autosave;

	/** EMS save time, as returned by Get Named Slot Info. Renaming does not change it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Saves")
	FDateTime TimeStamp;
};

/** Persistent metadata identifying the latest checkpoint for an EMS save slot. */
USTRUCT(BlueprintType)
struct EMSADDONSAUTOSAVE_API FEMSCheckpointRecord
{
	GENERATED_BODY()

	/** Version 2: checkpoints are stored in their own EMS slot (Save Slot). */
	static constexpr int32 CurrentVersion = 2;

	/** Version 1 (1.1): the checkpoint is the slot that holds the record. Migrated when read. */
	static constexpr int32 LegacyVersion = 1;

	/** Serialized checkpoint-record version used to reject incompatible metadata. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Record")
	int32 Version = CurrentVersion;

	/** Persistent GUID of the checkpoint actor that produced this record. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Record")
	FGuid CheckpointId;

	/** Optional save-menu display name copied from the checkpoint actor. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Record")
	FText DisplayName;

	/** World asset containing the checkpoint. Used when checkpoint loading requires map travel. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Record")
	FSoftObjectPath World;

	/** UTC time at which the checkpoint activation was accepted. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Record")
	FDateTime ActivatedAt;

	/** EMS slot that holds this checkpoint's save. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Record")
	FString SaveSlot;

	/** Returns true when this record contains a supported version, checkpoint ID, world and slot. */
	bool IsValid() const
	{
		return Version == CurrentVersion && CheckpointId.IsValid() && !World.IsNull() && !SaveSlot.IsEmpty();
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

	/** The same saved snapshot, rather than a later activation of this actor. */
	bool HasSameActivation(const FEMSCheckpointRecord& Other) const
	{
		return HasSameIdentity(Other) && ActivatedAt == Other.ActivatedAt
			&& SaveSlot.Equals(Other.SaveSlot, ESearchCase::IgnoreCase);
	}
};
