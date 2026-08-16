//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSInstanceManager.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "WorldPartition/HLOD/HLODInstancedStaticMeshComponent.h"

namespace
{
	const FName FoliagePackageName(TEXT("/Script/Foliage"));
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
	Id.OwnerPath = FSoftObjectPath(SourceOwner);
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

	FoliageActor->ForEachFoliageInfo(
		[this, FoliageActor](UFoliageType* FoliageType, FFoliageInfo& Info)
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
			SourceId.OwnerPath = FSoftObjectPath(FoliageActor);
			// A foliage type owned by the Instanced Foliage Actor has the level in
			// its path, so this name carries the PIE prefix for the same reason
			// the level identity does.
			SourceId.SourceName = FName(
				*UWorld::RemovePIEPrefix(FoliageType->GetPathName()));
			SourceId.MeshPath = FSoftObjectPath(Mesh);
			// Painted foliage is authored and keyed in world space, which the
			// shared engine derives from this kind.
			SourceId.SourceKind = EEMSInstanceSourceKind::Foliage;

			RegisterSource(SourceId, Component);
			return true;
		});
}

void AEMSInstanceManager::CollectSources()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EMSAddons_CollectInstancedSources);
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	TArray<UInstancedStaticMeshComponent*> Components;
	for (ULevel* Level : World->GetLevels())
	{
		if (!Level)
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

			// An Instanced Foliage Actor owns nothing but Foliage-module
			// components, every one of which the component pass rejects, so it is
			// handled entirely by the foliage pass. Running both over it would
			// only count its owner stability a second time.
			if (AInstancedFoliageActor* FoliageActor =
				Cast<AInstancedFoliageActor>(Actor))
			{
				CollectFoliageSources(FoliageActor);
				continue;
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
