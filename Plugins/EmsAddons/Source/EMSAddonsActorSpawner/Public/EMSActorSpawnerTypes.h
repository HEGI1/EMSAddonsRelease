//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPtr.h"
#include "EMSActorSpawnerTypes.generated.h"

USTRUCT(BlueprintType)
struct EMSADDONSACTORSPAWNER_API FEMSSpawnedActorId
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EMS Addons|Actor Spawner")
	FGuid Guid;

	FEMSSpawnedActorId() = default;

	explicit FEMSSpawnedActorId(const FGuid& InGuid)
		: Guid(InGuid)
	{
	}

	bool IsValid() const
	{
		return Guid.IsValid();
	}

	FString ToString() const
	{
		return Guid.ToString(EGuidFormats::Digits);
	}

	bool operator==(const FEMSSpawnedActorId& Other) const
	{
		return Guid == Other.Guid;
	}

	friend uint32 GetTypeHash(const FEMSSpawnedActorId& ActorId)
	{
		return GetTypeHash(ActorId.Guid);
	}
};

/**
 * Why the spawner is destroying one of its own actors.
 *
 * Only the cases that behave differently are modelled. Everything the spawner
 * does on its own initiative is Internal: what those all have in common, and
 * all the destroyed handler needs, is that the removal is not gameplay's doing
 * and must not be recorded as one.
 */
UENUM()
enum class EEMSSpawnerDestructionContext : uint8
{
	None,
	/** Marks the record absent and broadcasts On Actor Removed. */
	Gameplay,
	/** Releases the stable name first, so the correct class can take it. */
	ReplaceClass,
	Internal
};

USTRUCT(BlueprintType)
struct EMSADDONSACTORSPAWNER_API FEMSSpawnedActorRecord
{
	GENERATED_BODY()

	static constexpr int32 CurrentVersion = 1;

	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Actor Spawner")
	FGuid LocalId;

	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Actor Spawner")
	FSoftClassPath ActorClass;

	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Actor Spawner")
	FTransform SavedTransform = FTransform::Identity;

	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Actor Spawner")
	bool bExists = true;

	/**
	 * Restores attachment to the spawner itself.
	 *
	 * Attachment between two spawned actors is deliberately not persisted. The
	 * spawner's job is to put an actor back in the right level with its own
	 * state; reconstructing a hierarchy between siblings is gameplay's, and it
	 * can do so from OnActorRestored once every actor exists.
	 */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Actor Spawner")
	bool bAttachToSpawner = false;

	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Actor Spawner")
	FName AttachSocket;

	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Actor Spawner")
	FTransform RelativeAttachmentTransform = FTransform::Identity;

	/** Opaque FGameObjectSaveData produced by EMS SaveActorToBinary. */
	UPROPERTY(SaveGame)
	TArray<uint8> ActorBinaryData;

	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Actor Spawner")
	int32 RecordVersion = CurrentVersion;
};
