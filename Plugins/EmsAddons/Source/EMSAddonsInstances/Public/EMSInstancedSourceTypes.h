//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/SoftObjectPath.h"
#include "EMSInstancedSourceTypes.generated.h"

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

	UPROPERTY(SaveGame)
	FSoftObjectPath OwnerPath;

	UPROPERTY(SaveGame)
	FName SourceName;

	UPROPERTY(SaveGame)
	FSoftObjectPath MeshPath;

	UPROPERTY(SaveGame)
	EEMSInstanceSourceKind SourceKind = EEMSInstanceSourceKind::ISM;

	bool IsValid() const
	{
		return !LevelIdentity.IsNone()
			&& OwnerPath.IsValid()
			&& !SourceName.IsNone()
			&& MeshPath.IsValid();
	}

	bool operator==(const FEMSInstanceSourceId& Other) const
	{
		return LevelIdentity == Other.LevelIdentity
			&& OwnerPath == Other.OwnerPath
			&& SourceName == Other.SourceName
			&& MeshPath == Other.MeshPath
			&& SourceKind == Other.SourceKind;
	}

	FString ToString() const
	{
		return FString::Printf(
			TEXT("%s|%s|%s|%s|%d"),
			*LevelIdentity.ToString(),
			*OwnerPath.ToString(),
			*SourceName.ToString(),
			*MeshPath.ToString(),
			static_cast<int32>(SourceKind));
	}

	friend uint32 GetTypeHash(const FEMSInstanceSourceId& Id)
	{
		uint32 Hash = GetTypeHash(Id.LevelIdentity);
		Hash = HashCombine(Hash, GetTypeHash(Id.OwnerPath));
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

/**
 * One gameplay float on an instance, addressed by gameplay tag.
 *
 * This is deliberately separate from per-instance custom data. Custom data is
 * uploaded to the material and is limited to whatever float count the component
 * declares, whereas this carries pure gameplay state such as remaining tree
 * health and never touches rendering.
 */
USTRUCT(BlueprintType)
struct EMSADDONSINSTANCES_API FEMSInstanceGameplayValue
{
	GENERATED_BODY()

	UPROPERTY(
		SaveGame,
		EditAnywhere,
		BlueprintReadWrite,
		Category = "EMS Addons|Instanced")
	FGameplayTag Tag;

	UPROPERTY(
		SaveGame,
		EditAnywhere,
		BlueprintReadWrite,
		Category = "EMS Addons|Instanced")
	float Value = 0.0f;
};

/** The sparse set of gameplay values carried by a single instance. */
USTRUCT(BlueprintType)
struct EMSADDONSINSTANCES_API FEMSInstanceGameplayData
{
	GENERATED_BODY()

	UPROPERTY(
		SaveGame,
		EditAnywhere,
		BlueprintReadWrite,
		Category = "EMS Addons|Instanced")
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
