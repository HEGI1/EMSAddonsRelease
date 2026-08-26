//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/SoftObjectPath.h"
#include "EMSInstancedSourceTypes.generated.h"

class UStaticMesh;

namespace EMSAddons
{
	/** Actors and components carrying this tag are excluded from instanced-source persistence. */
	static const FName IgnoreInstancesTag(TEXT("EMS.IgnoreInstances"));
}

UENUM()
enum class EEMSInstanceSourceKind : uint8
{
	ISM,
	HISM,
	Foliage
};

USTRUCT()
struct EMSADDONSINSTANCES_API FEMSInstanceSourceId
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FName LevelIdentity;

	/**
	 * Conventional-level fallback only.
	 *
	 * World Partition runtime-cell actors deliberately leave this empty because
	 * their object paths live in transient /Memory packages. Their persistent
	 * identity is OwnerInstanceGuid instead.
	 */
	UPROPERTY(SaveGame)
	FSoftObjectPath OwnerPath;

	UPROPERTY(SaveGame)
	FName SourceName;

	UPROPERTY(SaveGame)
	FSoftObjectPath MeshPath;

	UPROPERTY(SaveGame)
	EEMSInstanceSourceKind SourceKind = EEMSInstanceSourceKind::ISM;

	/** Stable placed-actor identity used by World Partition and level instances. */
	UPROPERTY(SaveGame)
	FGuid OwnerInstanceGuid;

	bool HasStableOwnerIdentity() const
	{
		// World Partition sources carry the GUID and never a path. Conventional
		// levels carry the path, which is stable there.
		return OwnerInstanceGuid.IsValid() || OwnerPath.IsValid();
	}

	bool IsValid() const
	{
		return !LevelIdentity.IsNone()
			&& HasStableOwnerIdentity()
			&& !SourceName.IsNone()
			&& MeshPath.IsValid();
	}

	bool operator==(const FEMSInstanceSourceId& Other) const
	{
		if (LevelIdentity != Other.LevelIdentity
			|| SourceName != Other.SourceName
			|| MeshPath != Other.MeshPath
			|| SourceKind != Other.SourceKind)
		{
			return false;
		}

		// A GUID identity never falls back to comparing an object path. This keeps
		// a transient path from accidentally matching a stable source.
		if (OwnerInstanceGuid.IsValid() || Other.OwnerInstanceGuid.IsValid())
		{
			return OwnerInstanceGuid == Other.OwnerInstanceGuid;
		}

		return OwnerPath == Other.OwnerPath;
	}

	FString ToString() const
	{
		const FString OwnerIdentity = OwnerInstanceGuid.IsValid()
			? FString::Printf(
				TEXT("ActorGuid:%s"),
				*OwnerInstanceGuid.ToString(EGuidFormats::Digits))
			: FString::Printf(TEXT("ActorPath:%s"), *OwnerPath.ToString());

		return FString::Printf(
			TEXT("%s|%s|%s|%s|%d"),
			*LevelIdentity.ToString(),
			*OwnerIdentity,
			*SourceName.ToString(),
			*MeshPath.ToString(),
			static_cast<int32>(SourceKind));
	}

	friend uint32 GetTypeHash(const FEMSInstanceSourceId& Id)
	{
		uint32 Hash = GetTypeHash(Id.LevelIdentity);
		Hash = HashCombine(
			Hash,
			Id.OwnerInstanceGuid.IsValid()
				? GetTypeHash(Id.OwnerInstanceGuid)
				: GetTypeHash(Id.OwnerPath));
		Hash = HashCombine(Hash, GetTypeHash(Id.SourceName));
		Hash = HashCombine(Hash, GetTypeHash(Id.MeshPath));
		return HashCombine(Hash, GetTypeHash(static_cast<uint8>(Id.SourceKind)));
	}
};

USTRUCT()
struct EMSADDONSINSTANCES_API FEMSInstanceKey
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	uint64 TransformHash = 0;

	UPROPERTY(SaveGame)
	uint16 DuplicateOrdinal = 0;

	bool operator==(const FEMSInstanceKey& Other) const
	{
		return TransformHash == Other.TransformHash
			&& DuplicateOrdinal == Other.DuplicateOrdinal;
	}

	friend uint32 GetTypeHash(const FEMSInstanceKey& Key)
	{
		return HashCombine(
			GetTypeHash(Key.TransformHash),
			GetTypeHash(Key.DuplicateOrdinal));
	}
};

/** One gameplay float on an instance, addressed by gameplay tag. */
USTRUCT()
struct EMSADDONSINSTANCES_API FEMSInstanceGameplayValue
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FGameplayTag Tag;

	UPROPERTY(SaveGame)
	float Value = 0.0f;
};

/** Internal sparse set of gameplay values carried by a single instance. */
USTRUCT()
struct EMSADDONSINSTANCES_API FEMSInstanceGameplayData
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TArray<FEMSInstanceGameplayValue> Values;

	bool IsEmpty() const
	{
		return Values.IsEmpty();
	}

	const float* Find(const FGameplayTag& Tag) const
	{
		for (const FEMSInstanceGameplayValue& Entry : Values)
		{
			if (Entry.Tag == Tag)
			{
				return &Entry.Value;
			}
		}
		return nullptr;
	}

	void Set(const FGameplayTag& Tag, const float Value)
	{
		for (FEMSInstanceGameplayValue& Entry : Values)
		{
			if (Entry.Tag == Tag)
			{
				Entry.Value = Value;
				return;
			}
		}

		FEMSInstanceGameplayValue& Added = Values.AddDefaulted_GetRef();
		Added.Tag = Tag;
		Added.Value = Value;
	}

	bool Remove(const FGameplayTag& Tag)
	{
		return Values.RemoveAll(
			[&Tag](const FEMSInstanceGameplayValue& Entry)
			{
				return Entry.Tag == Tag;
			}) > 0;
	}
};

/**
 * High-level replacement settings used by Replace Instance.
 *
 * Transform Offset is applied in the original instance's local space. By
 * default the original per-instance scale is preserved, including foliage
 * random scale. Disable Preserve Scale to start from 1,1,1 before the offset is
 * composed, making Transform Offset.Scale the replacement's effective scale.
 */
USTRUCT(BlueprintType)
struct EMSADDONSINSTANCES_API FEMSInstanceReplacement
{
	GENERATED_BODY()

	/** Static Mesh used by the replacement instance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replacement")
	TObjectPtr<UStaticMesh> Mesh = nullptr;

	/** Optional local transform adjustment applied to the replacement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replacement")
	FTransform TransformOffset = FTransform::Identity;

	/** Preserve the original instance's scale before applying Transform Offset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replacement")
	bool bPreserveScale = true;
};

/**
 * One runtime-created replacement source.
 *
 * Replacement sources are recreated before discovery when their authored
 * template becomes available, so an automatically created target survives
 * ordinary level streaming and a later EMS load without Blueprint setup.
 */
USTRUCT()
struct EMSADDONSINSTANCES_API FEMSInstanceReplacementSource
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FEMSInstanceSourceId TemplateSourceId;

	UPROPERTY(SaveGame)
	FSoftObjectPath ReplacementMeshPath;

	UPROPERTY(SaveGame)
	FName GeneratedSourceName;

	bool IsValid() const
	{
		return TemplateSourceId.IsValid()
			&& ReplacementMeshPath.IsValid()
			&& !GeneratedSourceName.IsNone();
	}
};

USTRUCT()
struct EMSADDONSINSTANCES_API FEMSInstanceState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FTransform3f Transform;

	UPROPERTY(SaveGame)
	TArray<float> CustomData;

	UPROPERTY(SaveGame)
	FEMSInstanceGameplayData GameplayData;
};

USTRUCT()
struct EMSADDONSINSTANCES_API FEMSInstanceCustomDataDelta
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FEMSInstanceKey InstanceKey;

	UPROPERTY(SaveGame)
	TArray<float> CustomData;
};

/**
 * Gameplay data attached to a baseline instance.
 *
 * Authored instances carry no gameplay data, so every entry here is a runtime
 * addition rather than a difference against an authored value.
 */
USTRUCT()
struct EMSADDONSINSTANCES_API FEMSInstanceGameplayDataDelta
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FEMSInstanceKey InstanceKey;

	UPROPERTY(SaveGame)
	FEMSInstanceGameplayData GameplayData;
};

/** Sparse difference between one source's authored baseline and its saved state. */
USTRUCT()
struct EMSADDONSINSTANCES_API FEMSInstanceSourceDelta
{
	GENERATED_BODY()

	static constexpr int32 CurrentVersion = 1;

	UPROPERTY(SaveGame)
	int32 Version = CurrentVersion;

	UPROPERTY(SaveGame)
	FEMSInstanceSourceId SourceId;

	UPROPERTY(SaveGame)
	uint64 BaselineSignature = 0;

	UPROPERTY(SaveGame)
	int32 BaselineCount = 0;

	UPROPERTY(SaveGame)
	int32 CustomDataFloatCount = 0;

	UPROPERTY(SaveGame)
	TArray<FEMSInstanceKey> RemovedInstances;

	UPROPERTY(SaveGame)
	TArray<FEMSInstanceState> AddedInstances;

	UPROPERTY(SaveGame)
	TArray<FEMSInstanceCustomDataDelta> ModifiedCustomData;

	UPROPERTY(SaveGame)
	TArray<FEMSInstanceGameplayDataDelta> ModifiedGameplayData;

	bool IsEmpty() const
	{
		return RemovedInstances.IsEmpty()
			&& AddedInstances.IsEmpty()
			&& ModifiedCustomData.IsEmpty()
			&& ModifiedGameplayData.IsEmpty();
	}
};
