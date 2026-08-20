//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSInstanceManager.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"

bool AEMSInstanceManager::ReplaceInstanceWithSettings(
	UInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex,
	const FEMSInstanceReplacement& Replacement,
	UInstancedStaticMeshComponent*& OutReplacementComponent,
	int32& OutReplacementInstanceIndex)
{
	OutReplacementComponent = nullptr;
	OutReplacementInstanceIndex = INDEX_NONE;

	UStaticMesh* ReplacementMesh = Replacement.Mesh.Get();
	if (!ReplacementMesh
		|| !HasInstanceAuthority()
		|| !IsValid(Component)
		|| !Component->GetStaticMesh()
		|| InstanceIndex < 0
		|| InstanceIndex >= Component->GetInstanceCount())
	{
		return false;
	}

	// Blueprint success means an actual replacement happened. Returning the
	// original source here made a chained gameplay-value write succeed even
	// though no instance was replaced.
	if (Component->GetStaticMesh() == ReplacementMesh
		&& Replacement.bPreserveScale
		&& Replacement.TransformOffset.Equals(FTransform::Identity))
	{
		return false;
	}

	FEMSInstanceSourceId SourceId;
	if (!FindRegisteredSourceId(Component, SourceId))
	{
		RefreshLoadedSources();
		if (!FindRegisteredSourceId(Component, SourceId))
		{
			return false;
		}
	}

	FTransform ReplacementWorldTransform;
	if (!Component->GetInstanceTransform(
		InstanceIndex,
		ReplacementWorldTransform,
		true))
	{
		return false;
	}

	if (!Replacement.bPreserveScale)
	{
		ReplacementWorldTransform.SetScale3D(FVector::OneVector);
	}

	// FTransform multiplication applies the left transform first. Composing the
	// offset before the world transform therefore applies it in instance-local
	// space, matching the Blueprint Compose Transforms convention.
	ReplacementWorldTransform =
		Replacement.TransformOffset * ReplacementWorldTransform;

	TArray<float> CustomData;
	ReadInstanceCustomData(Component, InstanceIndex, CustomData);

	FEMSInstanceGameplayData GameplayData;
	const bool bHasGameplayData = GetInstanceGameplayData(
		Component,
		InstanceIndex,
		GameplayData);

	UInstancedStaticMeshComponent* ReplacementComponent =
		GetOrCreateReplacementSource(
			SourceId,
			Component,
			ReplacementMesh);
	if (!ReplacementComponent)
	{
		return false;
	}

	// Discover a newly generated target while it is still empty so its authored
	// baseline stays zero. The normal sparse delta then stores the transformed
	// replacement exactly like any other runtime-added instance.
	RefreshLoadedSources();

	FEMSInstanceSourceId ReplacementSourceId;
	if (!FindRegisteredSourceId(
		ReplacementComponent,
		ReplacementSourceId)
		|| ReplacementComponent->NumCustomDataFloats !=
			Component->NumCustomDataFloats)
	{
		return false;
	}

	const int32 ReplacementIndex =
		ReplacementComponent->AddInstance(
			ReplacementWorldTransform,
			true);
	if (ReplacementIndex == INDEX_NONE)
	{
		return false;
	}

	if (!CustomData.IsEmpty()
		&& !ReplacementComponent->SetCustomData(
			ReplacementIndex,
			CustomData,
			true))
	{
		ReplacementComponent->RemoveInstance(ReplacementIndex);
		return false;
	}

	if (bHasGameplayData
		&& !GameplayData.IsEmpty()
		&& !SetInstanceGameplayData(
			ReplacementComponent,
			ReplacementIndex,
			GameplayData))
	{
		ReplacementComponent->RemoveInstance(ReplacementIndex);
		return false;
	}

	if (!Component->RemoveInstance(InstanceIndex))
	{
		if (bHasGameplayData && !GameplayData.IsEmpty())
		{
			ClearInstanceGameplayData(
				ReplacementComponent,
				ReplacementIndex);
		}
		ReplacementComponent->RemoveInstance(ReplacementIndex);
		return false;
	}

	OutReplacementComponent = ReplacementComponent;
	OutReplacementInstanceIndex = ReplacementIndex;
	return true;
}
