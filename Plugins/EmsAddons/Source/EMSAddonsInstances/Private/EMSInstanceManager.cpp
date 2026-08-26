//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSInstanceManager.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Misc/Crc.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/UObjectGlobals.h"
#include "WorldPartition/ActorInstanceGuids.h"
#include "WorldPartition/HLOD/HLODInstancedStaticMeshComponent.h"

namespace
{
	const FName FoliagePackageName(TEXT("/Script/Foliage"));

	FName MakeStableFoliageSourceName(const UFoliageType* FoliageType)
	{
		if (!FoliageType)
		{
			return NAME_None;
		}

		const FString NormalizedPath =
			UWorld::RemovePIEPrefix(FoliageType->GetPathName());
		if (!NormalizedPath.StartsWith(TEXT("/Memory/")))
		{
			return FName(*NormalizedPath);
		}

		// Embedded/generated foliage types can inherit the runtime-cell package in
		// their path. The stable world + actor + local object name + mesh identity
		// is sufficient and survives recreation of that /Memory package.
		return FoliageType->GetFName();
	}
}

bool AEMSInstanceManager::IsLevelReadyForSourceDiscovery(const ULevel* Level)
{
	if (!Level)
	{
		return false;
	}

	const UWorld* World = Level->GetWorld();
	if (World && Level == World->PersistentLevel)
	{
		return true;
	}

	// A full manager refresh can run because a different cell finished loading.
	// Do not let that pass discover a second cell while Unreal is still assembling
	// its actors/components, or that partial state becomes the cached authored
	// baseline and every later delta correctly fails validation against it.
	//
	// bAlreadyUpdatedComponents and its siblings are temporary bookkeeping that
	// Unreal keeps only while making a level visible and clears once that pass
	// finishes, so a level that has fully streamed in reads false. Level
	// visibility with no transition pending is the state that actually means
	// "assembled and in the world".
	return Level->bIsVisible
		&& !Level->HasVisibilityChangeRequestPending()
		&& Level->bAreComponentsCurrentlyRegistered;
}

bool AEMSInstanceManager::IsSupportedActor(const AActor* Actor) const
{
	return IsValid(Actor)
		&& !Actor->IsEditorOnly()
		&& !Actor->ActorHasTag(EMSAddons::IgnoreInstancesTag);
}

bool AEMSInstanceManager::IsSupportedSource(
	const UInstancedStaticMeshComponent* Component) const
{
	const UPackage* ClassPackage =
		Component ? Component->GetClass()->GetPackage() : nullptr;
	// Anything the Foliage module owns is excluded here even though this manager
	// now persists painted foliage as well. Those components are registered by
	// the foliage pass under their world-space identity, and an Instanced Foliage
	// Actor is an ordinary actor to the component pass, so without this check the
	// same component would be registered twice under two identities keyed in two
	// different spaces. It also keeps Landscape Grass out, which is unsupported.
	return IsValid(Component)
		&& Component->IsRegistered()
		&& Component->GetStaticMesh()
		&& !Component->HasAnyFlags(RF_Transient)
		&& !Component->IsEditorOnly()
		&& !Component->ComponentHasTag(EMSAddons::IgnoreInstancesTag)
		&& !Component->IsA<UHLODInstancedStaticMeshComponent>()
		&& (!ClassPackage || ClassPackage->GetFName() != FoliagePackageName);
}

FEMSInstanceSourceId AEMSInstanceManager::BuildSourceId(
	const UInstancedStaticMeshComponent* Component) const
{
	FEMSInstanceSourceId Id;
	if (!Component || !Component->GetOwner() || !Component->GetStaticMesh())
	{
		return Id;
	}

	const AActor* SourceOwner = Component->GetOwner();
	Id.LevelIdentity = MakeLevelIdentity(SourceOwner->GetLevel());
	if (!BuildStableOwnerIdentity(
			SourceOwner,
			Id.OwnerInstanceGuid,
			Id.OwnerPath))
	{
		return FEMSInstanceSourceId();
	}
	Id.SourceName = Component->GetFName();
	Id.MeshPath = FSoftObjectPath(Component->GetStaticMesh());
	Id.SourceKind = Component->IsA<UHierarchicalInstancedStaticMeshComponent>()
		? EEMSInstanceSourceKind::HISM
		: EEMSInstanceSourceKind::ISM;
	return Id;
}

void AEMSInstanceManager::CollectFoliageSources(
	AInstancedFoliageActor* FoliageActor)
{
	// Owner stability is checked before the types are walked because an Instanced
	// Foliage Actor that has one has them all, unlike an ordinary actor which may
	// carry no instanced components at all.
	if (!IsStableSourceOwner(FoliageActor))
	{
		return;
	}

	FGuid OwnerInstanceGuid;
	FSoftObjectPath OwnerPath;
	if (!BuildStableOwnerIdentity(
			FoliageActor,
			OwnerInstanceGuid,
			OwnerPath))
	{
		return;
	}

	FoliageActor->ForEachFoliageInfo(
		[this, FoliageActor, OwnerInstanceGuid, OwnerPath](
			UFoliageType* FoliageType,
			FFoliageInfo& Info)
		{
			UFoliageType_InstancedStaticMesh* StaticMeshType =
				Cast<UFoliageType_InstancedStaticMesh>(FoliageType);
			UStaticMesh* Mesh = StaticMeshType
				? StaticMeshType->GetStaticMesh()
				: nullptr;
			UHierarchicalInstancedStaticMeshComponent* Component =
				Info.GetComponent();
			if (!Mesh
				|| !IsValid(Component)
				|| !Component->IsRegistered()
				|| Component->ComponentHasTag(EMSAddons::IgnoreInstancesTag))
			{
				return true;
			}

			FEMSInstanceSourceId SourceId;
			SourceId.LevelIdentity = MakeLevelIdentity(FoliageActor->GetLevel());
			SourceId.OwnerInstanceGuid = OwnerInstanceGuid;
			SourceId.OwnerPath = OwnerPath;
			SourceId.SourceName = MakeStableFoliageSourceName(FoliageType);
			SourceId.MeshPath = FSoftObjectPath(Mesh);
			// Painted foliage is authored and keyed in world space, which the
			// shared engine derives from this kind.
			SourceId.SourceKind = EEMSInstanceSourceKind::Foliage;

			RegisterSource(SourceId, Component);
			return true;
		});
}

AActor* AEMSInstanceManager::FindSourceOwner(
	const FEMSInstanceSourceId& SourceId) const
{
	if (!SourceId.IsValid())
	{
		return nullptr;
	}

	// Conventional levels keep their stable soft-object path as a cheap direct
	// lookup. World Partition deliberately leaves it empty and resolves through
	// the actor-instance GUID inside the matching stable world scope.
	if (!SourceId.OwnerInstanceGuid.IsValid())
	{
		return Cast<AActor>(SourceId.OwnerPath.ResolveObject());
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	for (ULevel* Level : World->GetLevels())
	{
		if (!IsLevelReadyForSourceDiscovery(Level)
			|| MakeLevelIdentity(Level) != SourceId.LevelIdentity)
		{
			continue;
		}

		for (AActor* Actor : Level->Actors)
		{
			if (IsValid(Actor)
				&& FActorInstanceGuid::GetActorInstanceGuid(*Actor)
					== SourceId.OwnerInstanceGuid)
			{
				return Actor;
			}
		}
	}
	return nullptr;
}

UInstancedStaticMeshComponent* AEMSInstanceManager::FindTemplateComponent(
	const FEMSInstanceSourceId& SourceId) const
{
	if (!SourceId.IsValid())
	{
		return nullptr;
	}

	AActor* SourceOwner = FindSourceOwner(SourceId);
	if (!SourceOwner)
	{
		return nullptr;
	}

	if (SourceId.SourceKind == EEMSInstanceSourceKind::Foliage)
	{
		AInstancedFoliageActor* FoliageActor =
			Cast<AInstancedFoliageActor>(SourceOwner);
		if (!FoliageActor)
		{
			return nullptr;
		}

		UInstancedStaticMeshComponent* FoundComponent = nullptr;
		FoliageActor->ForEachFoliageInfo(
			[&](UFoliageType* FoliageType, FFoliageInfo& Info)
			{
				UFoliageType_InstancedStaticMesh* StaticMeshType =
					Cast<UFoliageType_InstancedStaticMesh>(FoliageType);
				UStaticMesh* Mesh = StaticMeshType
					? StaticMeshType->GetStaticMesh()
					: nullptr;
				if (!Mesh || FSoftObjectPath(Mesh) != SourceId.MeshPath)
				{
					return true;
				}

				if (MakeStableFoliageSourceName(FoliageType) == SourceId.SourceName)
				{
					FoundComponent = Info.GetComponent();
					return false;
				}
				return true;
			});
		return FoundComponent;
	}

	TArray<UInstancedStaticMeshComponent*> Components;
	SourceOwner->GetComponents(Components);
	for (UInstancedStaticMeshComponent* Component : Components)
	{
		if (!Component
			|| Component->GetFName() != SourceId.SourceName
			|| !Component->GetStaticMesh()
			|| FSoftObjectPath(Component->GetStaticMesh()) != SourceId.MeshPath)
		{
			continue;
		}

		const EEMSInstanceSourceKind Kind =
			Component->IsA<UHierarchicalInstancedStaticMeshComponent>()
				? EEMSInstanceSourceKind::HISM
				: EEMSInstanceSourceKind::ISM;
		if (Kind == SourceId.SourceKind)
		{
			return Component;
		}
	}
	return nullptr;
}

UClass* AEMSInstanceManager::GetReplacementComponentClass(
	const UInstancedStaticMeshComponent* SourceComponent)
{
	if (!SourceComponent)
	{
		return nullptr;
	}

	// A generated target is always a plain engine component, never a Foliage
	// module one. Discovery deliberately ignores that module so a component is
	// never registered under two identities, and the engine offers no runtime
	// way to add a Foliage Type to an Instanced Foliage Actor at all - every
	// entry point for that is editor-only. Keeping the target outside the
	// foliage system is also what leaves the authored Foliage Type untouched.
	const UPackage* ClassPackage = SourceComponent->GetClass()->GetPackage();
	if (ClassPackage && ClassPackage->GetFName() == FoliagePackageName)
	{
		return UHierarchicalInstancedStaticMeshComponent::StaticClass();
	}
	return SourceComponent->GetClass();
}

UInstancedStaticMeshComponent*
AEMSInstanceManager::FindCompatibleReplacementSource(
	const FEMSInstanceSourceId& SourceId,
	const UInstancedStaticMeshComponent* SourceComponent,
	UStaticMesh* ReplacementMesh) const
{
	AActor* SourceOwner = SourceComponent ? SourceComponent->GetOwner() : nullptr;
	const UClass* ComponentClass = GetReplacementComponentClass(SourceComponent);
	if (!SourceOwner || !ReplacementMesh || !ComponentClass)
	{
		return nullptr;
	}

	TArray<UInstancedStaticMeshComponent*> Components;
	SourceOwner->GetComponents(Components);
	for (UInstancedStaticMeshComponent* Component : Components)
	{
		if (Component == SourceComponent
			|| !IsSupportedSource(Component)
			|| Component->GetStaticMesh() != ReplacementMesh
			|| Component->GetClass() != ComponentClass
			|| Component->NumCustomDataFloats !=
				SourceComponent->NumCustomDataFloats)
		{
			continue;
		}

		// Authored targets can be shared. Runtime-generated targets stay owned by
		// the source whose descriptor recreates them after streaming or load.
		if (SavedReplacementSources.ContainsByPredicate(
				[&SourceId, Component](
					const FEMSInstanceReplacementSource& Descriptor)
				{
					return Descriptor.GeneratedSourceName == Component->GetFName()
						&& Descriptor.TemplateSourceId != SourceId;
				}))
		{
			continue;
		}
		return Component;
	}
	return nullptr;
}

FName AEMSInstanceManager::MakeReplacementSourceName(
	const FEMSInstanceSourceId& SourceId,
	const UStaticMesh* ReplacementMesh) const
{
	const FString Key = FString::Printf(
		TEXT("%s|%s|%d"),
		*SourceId.SourceName.ToString(),
		ReplacementMesh ? *ReplacementMesh->GetPathName() : TEXT("None"),
		static_cast<int32>(SourceId.SourceKind));
	return FName(*FString::Printf(
		TEXT("EMSReplacement_%08X"),
		FCrc::StrCrc32(*Key)));
}

UInstancedStaticMeshComponent* AEMSInstanceManager::EnsureReplacementSource(
	const FEMSInstanceReplacementSource& Descriptor,
	UInstancedStaticMeshComponent* KnownTemplate)
{
	if (!Descriptor.IsValid())
	{
		return nullptr;
	}

	UStaticMesh* ReplacementMesh =
		Cast<UStaticMesh>(Descriptor.ReplacementMeshPath.ResolveObject());
	if (!ReplacementMesh)
	{
		ReplacementMesh =
			Cast<UStaticMesh>(Descriptor.ReplacementMeshPath.TryLoad());
	}
	if (!ReplacementMesh)
	{
		return nullptr;
	}

	UInstancedStaticMeshComponent* SourceComponent = KnownTemplate
		? KnownTemplate
		: FindTemplateComponent(Descriptor.TemplateSourceId);
	AActor* SourceOwner = SourceComponent ? SourceComponent->GetOwner() : nullptr;
	UClass* ComponentClass = GetReplacementComponentClass(SourceComponent);
	if (!SourceOwner || !ComponentClass)
	{
		return nullptr;
	}

	TArray<UInstancedStaticMeshComponent*> Components;
	SourceOwner->GetComponents(Components);
	for (UInstancedStaticMeshComponent* Component : Components)
	{
		if (Component && Component->GetFName() == Descriptor.GeneratedSourceName)
		{
			// Older builds could put generated components in the instance list
			// without adding them to the actor's owned-component set. Adopt such a
			// component before returning it so the source manager can discover it
			// on the next refresh instead of creating a duplicate UObject.
			SourceOwner->AddOwnedComponent(Component);
			return Component->GetStaticMesh() == ReplacementMesh
				? Component
				: nullptr;
		}
	}
	for (UActorComponent* ActorComponent : SourceOwner->GetInstanceComponents())
	{
		UInstancedStaticMeshComponent* Component =
			Cast<UInstancedStaticMeshComponent>(ActorComponent);
		if (Component && Component->GetFName() == Descriptor.GeneratedSourceName)
		{
			SourceOwner->AddOwnedComponent(Component);
			return Component->GetStaticMesh() == ReplacementMesh
				? Component
				: nullptr;
		}
	}

	// The source only serves as an archetype when the generated component is of
	// its own class. A foliage source is deliberately replaced by a plain engine
	// component, and an archetype of a different class is not valid.
	UInstancedStaticMeshComponent* ReplacementComponent =
		NewObject<UInstancedStaticMeshComponent>(
			SourceOwner,
			ComponentClass,
			Descriptor.GeneratedSourceName,
			RF_NoFlags,
			ComponentClass == SourceComponent->GetClass()
				? SourceComponent
				: nullptr);
	if (!ReplacementComponent)
	{
		return nullptr;
	}

	ReplacementComponent->CreationMethod = EComponentCreationMethod::Instance;
	ReplacementComponent->ClearInstances();
	ReplacementComponent->SetStaticMesh(ReplacementMesh);
	// Set explicitly rather than inherited, because the foliage case has no
	// archetype to inherit it from.
	ReplacementComponent->SetNumCustomDataFloats(
		SourceComponent->NumCustomDataFloats);
	if (USceneComponent* AttachParent = SourceComponent->GetAttachParent())
	{
		ReplacementComponent->SetupAttachment(
			AttachParent,
			SourceComponent->GetAttachSocketName());
	}
	ReplacementComponent->SetRelativeTransform(
		SourceComponent->GetRelativeTransform());
	SourceOwner->AddInstanceComponent(ReplacementComponent);
	// AddInstanceComponent only records the component for instance cleanup. The
	// source manager discovers live components through OwnedComponents, so both
	// ownership registries must be updated for reconstruction to be idempotent.
	SourceOwner->AddOwnedComponent(ReplacementComponent);
	ReplacementComponent->OnComponentCreated();
	ReplacementComponent->RegisterComponent();
	return ReplacementComponent;
}

void AEMSInstanceManager::EnsureSavedReplacementSources()
{
	const int32 SourceCount = FMath::Min(
		SavedReplacementSources.Num(),
		MaxReplacementSources);
	if (SourceCount <= 0)
	{
		return;
	}

	int32 PreviousResolved = INDEX_NONE;
	for (int32 Pass = 0; Pass < SourceCount; ++Pass)
	{
		int32 Resolved = 0;
		for (int32 Index = 0; Index < SourceCount; ++Index)
		{
			const FEMSInstanceReplacementSource& Descriptor =
				SavedReplacementSources[Index];
			if (!Descriptor.IsValid()
				|| EnsureReplacementSource(Descriptor))
			{
				++Resolved;
			}
		}

		if (Resolved == SourceCount || Resolved == PreviousResolved)
		{
			break;
		}
		PreviousResolved = Resolved;
	}
}

UInstancedStaticMeshComponent*
AEMSInstanceManager::GetOrCreateReplacementSource(
	const FEMSInstanceSourceId& SourceId,
	UInstancedStaticMeshComponent* SourceComponent,
	UStaticMesh* ReplacementMesh)
{
	if (UInstancedStaticMeshComponent* Existing =
		FindCompatibleReplacementSource(
			SourceId,
			SourceComponent,
			ReplacementMesh))
	{
		return Existing;
	}

	const FSoftObjectPath ReplacementPath(ReplacementMesh);
	for (const FEMSInstanceReplacementSource& Descriptor :
		SavedReplacementSources)
	{
		if (Descriptor.TemplateSourceId == SourceId
			&& Descriptor.ReplacementMeshPath == ReplacementPath)
		{
			return EnsureReplacementSource(Descriptor, SourceComponent);
		}
	}

	if (SavedReplacementSources.Num() >= MaxReplacementSources)
	{
		return nullptr;
	}

	FEMSInstanceReplacementSource& Descriptor =
		SavedReplacementSources.AddDefaulted_GetRef();
	Descriptor.TemplateSourceId = SourceId;
	Descriptor.ReplacementMeshPath = ReplacementPath;
	Descriptor.GeneratedSourceName =
		MakeReplacementSourceName(SourceId, ReplacementMesh);

	UInstancedStaticMeshComponent* Created =
		EnsureReplacementSource(Descriptor, SourceComponent);
	if (!Created)
	{
		SavedReplacementSources.Pop();
	}
	return Created;
}

void AEMSInstanceManager::ReadInstanceCustomData(
	const UInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex,
	TArray<float>& OutCustomData)
{
	OutCustomData.Reset();
	if (!Component || Component->NumCustomDataFloats <= 0)
	{
		return;
	}

	OutCustomData.Init(0.0f, Component->NumCustomDataFloats);
	const int32 DataOffset = InstanceIndex * Component->NumCustomDataFloats;
	if (Component->PerInstanceSMCustomData.IsValidIndex(
		DataOffset + Component->NumCustomDataFloats - 1))
	{
		FMemory::Memcpy(
			OutCustomData.GetData(),
			Component->PerInstanceSMCustomData.GetData() + DataOffset,
			sizeof(float) * Component->NumCustomDataFloats);
	}
}

bool AEMSInstanceManager::ReplaceInstance(
	UInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex,
	UStaticMesh* ReplacementMesh)
{
	FEMSInstanceReplacement Replacement;
	Replacement.Mesh = ReplacementMesh;
	UInstancedStaticMeshComponent* ReplacementComponent = nullptr;
	int32 ReplacementIndex = INDEX_NONE;
	return ReplaceInstanceWithSettings(
		Component,
		InstanceIndex,
		Replacement,
		ReplacementComponent,
		ReplacementIndex);
}

void AEMSInstanceManager::CollectSources()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EMSAddons_CollectInstancedSources);
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Saved replacement descriptors are loaded on the persistent manager before
	// restore. Recreate their target sources first so this discovery pass sees an
	// empty authored baseline and the normal delta engine can restore additions.
	// FindSourceOwner itself refuses partially assembled streaming levels.
	EnsureSavedReplacementSources();

	TArray<UInstancedStaticMeshComponent*> Components;
	for (ULevel* Level : World->GetLevels())
	{
		if (!IsLevelReadyForSourceDiscovery(Level))
		{
			continue;
		}

		for (AActor* Actor : Level->Actors)
		{
			// The tag and editor-only checks gate both passes, so an actor the
			// project deliberately excluded is never reported as a skipped owner.
			if (!IsSupportedActor(Actor))
			{
				continue;
			}

			// An Instanced Foliage Actor owns Foliage-module components and is
			// handled through its foliage infos so those sources retain world-space
			// identity. It then falls through to the ordinary component pass rather
			// than being skipped, because generated replacement sources live on it
			// as plain engine components. IsSupportedSource excludes everything the
			// Foliage module owns, so no component is seen by both passes.
			if (AInstancedFoliageActor* FoliageActor =
				Cast<AInstancedFoliageActor>(Actor))
			{
				CollectFoliageSources(FoliageActor);
			}

			Components.Reset();
			Actor->GetComponents(Components);

			// Owner stability is checked last, and only once per owner that
			// actually has instanced components. Checking it first counted an
			// ignored or component-less actor as a rejected source owner, so the
			// skipped-owner warning fired for actors the project deliberately
			// excluded and scaled with component count rather than actor count.
			if (Components.IsEmpty() || !IsStableSourceOwner(Actor))
			{
				continue;
			}

			for (UInstancedStaticMeshComponent* Component : Components)
			{
				if (IsSupportedSource(Component))
				{
					RegisterSource(BuildSourceId(Component), Component);
				}
			}
		}
	}
}
