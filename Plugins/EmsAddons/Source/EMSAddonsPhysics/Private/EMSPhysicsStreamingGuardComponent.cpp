//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSPhysicsStreamingGuardComponent.h"

#include "Components/PrimitiveComponent.h"
#include "EMSAddonsPhysics.h"
#include "Engine/HitResult.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Streaming/LevelStreamingDelegates.h"
#include "TimerManager.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionLevelStreamingDynamic.h"
#include "WorldPartition/WorldPartitionRuntimeCell.h"
#include "WorldPartition/WorldPartitionStreamingSource.h"
#include "WorldPartition/WorldPartitionSubsystem.h"

namespace
{
	/**
	 * Probe reach is measured from slightly inside the bottom of a body's bounds.
	 *
	 * A body resting on a floor overlaps it by the solver's contact tolerance, so
	 * reach measured from exactly the bounds would start below the surface it is
	 * looking for.
	 */
	constexpr double EMSSupportProbeStartOffset = 2.0;

	/**
	 * How far toward the edge of a body's footprint the outer probe columns sit.
	 *
	 * Not all the way: the corners of an axis-aligned box lie outside a round or
	 * rotated body, and a column there would answer for floor the body never
	 * touches.
	 */
	constexpr double EMSSupportProbeFootprintFraction = 0.8;
}

UEMSPhysicsStreamingGuardComponent::UEMSPhysicsStreamingGuardComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bAutoActivate = true;
}

void UEMSPhysicsStreamingGuardComponent::BeginPlay()
{
	Super::BeginPlay();

	// A sublevel that is hidden and shown again routes End Play and then Begin
	// Play on the same component, so nothing may carry over from the last time.
	bSuspended = false;
	SuspendedBodies.Reset();
	DepartingLevels.Reset();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	BeginInvisibleHandle = FLevelStreamingDelegates::OnLevelBeginMakingInvisible.AddUObject(
		this, &UEMSPhysicsStreamingGuardComponent::HandleLevelBeginMakingInvisible);
	StateChangedHandle = FLevelStreamingDelegates::OnLevelStreamingStateChanged.AddUObject(
		this, &UEMSPhysicsStreamingGuardComponent::HandleLevelStreamingStateChanged);

	const UWorldPartition* WorldPartition = World->GetWorldPartition();
	const bool bStreamingWorld =
		(WorldPartition && WorldPartition->bEnableStreaming)
		|| !World->GetStreamingLevels().IsEmpty();
	if (bStreamingWorld)
	{
		// The cells around a placed actor are not visible yet on the frame it begins
		// play, so the opening check runs one tick later - still before gravity has
		// moved anything anywhere.
		InitialEvaluationTimer = World->GetTimerManager().SetTimerForNextTick(
			this,
			&UEMSPhysicsStreamingGuardComponent::EvaluateInitialSupport);
	}
}

void UEMSPhysicsStreamingGuardComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	FLevelStreamingDelegates::OnLevelBeginMakingInvisible.Remove(BeginInvisibleHandle);
	FLevelStreamingDelegates::OnLevelStreamingStateChanged.Remove(StateChangedHandle);
	BeginInvisibleHandle.Reset();
	StateChangedHandle.Reset();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(InitialEvaluationTimer);
	}

	// The physics flags belong to the owner and were only borrowed while the
	// ground was missing. Handing them back here is what keeps a hidden and shown
	// actor from coming back permanently unable to simulate.
	ResumeOwnerPhysics();
	DepartingLevels.Reset();

	Super::EndPlay(EndPlayReason);
}

void UEMSPhysicsStreamingGuardComponent::EvaluateInitialSupport()
{
	// A body over a real void - spawned above a chasm, thrown past the edge of the
	// map - has no support below it either. Only a world still streaming in below
	// it explains the gap; otherwise the body falls as it would anywhere else.
	if (!bSuspended && !HasAnySupportBelow() && !IsWorldBelowOwnerLoaded())
	{
		SuspendOwnerPhysics();
	}
}

void UEMSPhysicsStreamingGuardComponent::HandleLevelBeginMakingInvisible(
	UWorld* InWorld,
	const ULevelStreaming* StreamingLevel,
	ULevel* LoadedLevel)
{
	if (InWorld != GetWorld())
	{
		return;
	}
	if (LoadedLevel)
	{
		DepartingLevels.Add(LoadedLevel);
	}
	if (bSuspended)
	{
		return;
	}

	// This runs before the level's collision actually leaves the world, which is
	// the only moment the contact probe can still name the level holding the
	// owner up. Once it is invisible the owner is already unsupported and every
	// airborne actor in the world would look the same.
	if (IsSupportedByLevel(LoadedLevel))
	{
		SuspendOwnerPhysics();
	}
}

void UEMSPhysicsStreamingGuardComponent::HandleLevelStreamingStateChanged(
	UWorld* InWorld,
	const ULevelStreaming* StreamingLevel,
	ULevel* LoadedLevel,
	ELevelStreamingState PreviousState,
	ELevelStreamingState NewState)
{
	if (InWorld != GetWorld())
	{
		return;
	}
	if (LoadedLevel)
	{
		if (NewState == ELevelStreamingState::MakingInvisible)
		{
			DepartingLevels.Add(LoadedLevel);
		}
		else if (NewState == ELevelStreamingState::LoadedVisible
			|| NewState == ELevelStreamingState::LoadedNotVisible
			|| NewState == ELevelStreamingState::Unloaded
			|| NewState == ELevelStreamingState::Removed
			|| NewState == ELevelStreamingState::FailedToLoad)
		{
			DepartingLevels.Remove(LoadedLevel);
		}
	}
	for (auto It = DepartingLevels.CreateIterator(); It; ++It)
	{
		if (!It->IsValid()) It.RemoveCurrent();
	}
	if (!bSuspended)
	{
		return;
	}

	// The event's level need not own the support, so the probe decides. Nor is
	// LoadedVisible enough on its own: the opening check can suspend an actor a
	// tick before the cell under it finishes becoming visible, and that cell then
	// never reports the edge again. Something loaded below is enough - falling
	// onto it is ordinary physics, not the defect this guards against. Collision
	// from a departing level is excluded even while still registered. A world that
	// has finished loading below the owner and still holds nothing up is a real
	// void, so waiting longer would only keep the body hanging forever.
	if (HasAnySupportBelow() || IsWorldBelowOwnerLoaded())
	{
		ResumeOwnerPhysics();
	}
}

void UEMSPhysicsStreamingGuardComponent::GatherSupportProbeColumns(
	TArray<FEMSSupportProbeColumn>& OutColumns) const
{
	OutColumns.Reset();
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// Each simulating body is probed under its own footprint. The actor's bounds
	// as a whole say little about where anything rests: a broken Geometry
	// Collection's bounds span every scattered piece, and two bodies on one actor
	// can sit on different floors.
	TArray<FBox, TInlineAllocator<4>> BodyBounds;
	for (const UActorComponent* Component : Owner->GetComponents())
	{
		const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
		if (Primitive && Primitive->IsRegistered() && Primitive->BodyInstance.bSimulatePhysics)
		{
			BodyBounds.Add(Primitive->Bounds.GetBox());
		}
	}
	if (BodyBounds.IsEmpty())
	{
		FVector BoundsOrigin;
		FVector BoundsExtent;
		Owner->GetActorBounds(true, BoundsOrigin, BoundsExtent, false);
		BodyBounds.Add(FBox(BoundsOrigin - BoundsExtent, BoundsOrigin + BoundsExtent));
	}

	// The centre plus four columns toward the corners of the footprint. A wide
	// body resting partly on a platform has its centre over open space, and only
	// an outer column finds the floor actually holding it.
	static const FVector2D FootprintOffsets[] =
	{
		FVector2D(0.0, 0.0),
		FVector2D(1.0, 1.0),
		FVector2D(1.0, -1.0),
		FVector2D(-1.0, 1.0),
		FVector2D(-1.0, -1.0),
	};

	for (const FBox& Box : BodyBounds)
	{
		if (!Box.IsValid)
		{
			continue;
		}

		const FVector Center = Box.GetCenter();
		const FVector Extent = Box.GetExtent();
		const double ReachOriginZ = Box.Min.Z + EMSSupportProbeStartOffset;

		// Columns start at the body's centre height rather than at the bottom of
		// its bounds. On a slope or under a tilted body the floor below an outer
		// column is higher than the lowest point of the bounds, and a line that
		// begins inside that floor does not report it. The owner itself is ignored
		// by the query, so starting inside it costs nothing.
		const double StartZ = FMath::Max(Center.Z, ReachOriginZ);
		for (const FVector2D& Offset : FootprintOffsets)
		{
			FEMSSupportProbeColumn& Column = OutColumns.AddDefaulted_GetRef();
			Column.Start = FVector(
				Center.X + Offset.X * Extent.X * EMSSupportProbeFootprintFraction,
				Center.Y + Offset.Y * Extent.Y * EMSSupportProbeFootprintFraction,
				StartZ);
			Column.ReachOriginZ = ReachOriginZ;
		}
	}
}

bool UEMSPhysicsStreamingGuardComponent::TraceSupportColumn(
	const FEMSSupportProbeColumn& Column,
	const double Reach,
	FHitResult& OutHit,
	const bool bIgnoreDepartingLevels) const
{
	const AActor* Owner = GetOwner();
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World)
	{
		return false;
	}

	const FVector Start = Column.Start;
	const FVector End(Start.X, Start.Y, Column.ReachOriginZ - FMath::Max(Reach, 0.0));

	const FCollisionQueryParams QueryParams(
		SCENE_QUERY_STAT(EMSPhysicsStreamingGuard),
		false,
		Owner);
	const FCollisionObjectQueryParams ObjectParams(
		ECC_TO_BITFIELD(ECC_WorldStatic)
		| ECC_TO_BITFIELD(ECC_WorldDynamic)
		| ECC_TO_BITFIELD(ECC_PhysicsBody));

	if (!bIgnoreDepartingLevels)
	{
		return World->LineTraceSingleByObjectType(OutHit, Start, End, ObjectParams, QueryParams);
	}

	// An object multi-trace also finds usable geometry below a departing floor.
	// Rejecting just the first hit would otherwise leave the body suspended even
	// when a second, loaded floor can safely catch it.
	TArray<FHitResult> Hits;
	World->LineTraceMultiByObjectType(Hits, Start, End, ObjectParams, QueryParams);
	for (const FHitResult& Hit : Hits)
	{
		// BSP collision has no owning actor; the component level is its outer level.
		// A hit whose level cannot be named at all cannot be departing either.
		const UPrimitiveComponent* SupportComponent = Hit.GetComponent();
		ULevel* SupportLevel = SupportComponent ? SupportComponent->GetComponentLevel() : nullptr;
		if (!SupportLevel || (!SupportLevel->bIsBeingRemoved && !DepartingLevels.Contains(SupportLevel)))
		{
			OutHit = Hit;
			return true;
		}
	}
	return false;
}

bool UEMSPhysicsStreamingGuardComponent::HasAnySupportBelow() const
{
	TArray<FEMSSupportProbeColumn> Columns;
	GatherSupportProbeColumns(Columns);
	if (Columns.IsEmpty())
	{
		return false;
	}

	// Every column must find loaded collision. A body straddling two cells while
	// one of them is away still has support under its other half, and releasing
	// it then would tip it into the gap the guard exists to bridge.
	for (const FEMSSupportProbeColumn& Column : Columns)
	{
		FHitResult Hit;
		if (!TraceSupportColumn(Column, MissingWorldProbeDepth, Hit, true))
		{
			return false;
		}
	}
	return true;
}

bool UEMSPhysicsStreamingGuardComponent::IsSupportedByLevel(const ULevel* Level) const
{
	if (!Level)
	{
		return false;
	}

	TArray<FEMSSupportProbeColumn> Columns;
	GatherSupportProbeColumns(Columns);

	// One column resting on the level is enough: losing any part of what holds a
	// body up can drop or tip it.
	for (const FEMSSupportProbeColumn& Column : Columns)
	{
		FHitResult Hit;
		if (!TraceSupportColumn(Column, SupportContactReach, Hit, false))
		{
			continue;
		}

		// The component level, not the owner's, so BSP floors name their level too.
		const UPrimitiveComponent* HitComponent = Hit.GetComponent();
		if (HitComponent && HitComponent->GetComponentLevel() == Level)
		{
			return true;
		}
	}
	return false;
}

bool UEMSPhysicsStreamingGuardComponent::IsWorldBelowOwnerLoaded() const
{
	const AActor* Owner = GetOwner();
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World)
	{
		return true;
	}

	// A level still on its way out has not finished changing what lies below.
	for (const TWeakObjectPtr<ULevel>& Departing : DepartingLevels)
	{
		if (Departing.IsValid())
		{
			return false;
		}
	}

	// Conventional sublevels and unembedded level instances have no location the
	// guard could test against, so any of them that is not fully visible might be
	// the missing floor. World Partition cells are answered spatially below.
	for (const ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
	{
		if (!StreamingLevel || StreamingLevel->IsA<UWorldPartitionLevelStreamingDynamic>())
		{
			continue;
		}
		if (StreamingLevel->GetLevelStreamingState() != ELevelStreamingState::LoadedVisible)
		{
			return false;
		}
	}

	const UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (WorldPartition && WorldPartition->bEnableStreaming)
	{
		const UWorldPartitionSubsystem* Subsystem = World->GetSubsystem<UWorldPartitionSubsystem>();
		if (!Subsystem)
		{
			return false;
		}

		// Every cell overlapping the owner's footprint must be active. Where no
		// cell exists at all there is nothing left to stream, which is exactly the
		// empty space over a real void. Cells held back by an inactive data layer
		// are skipped by the query, so they count as loaded too.
		FVector BoundsOrigin;
		FVector BoundsExtent;
		Owner->GetActorBounds(true, BoundsOrigin, BoundsExtent, false);

		FWorldPartitionStreamingQuerySource QuerySource(BoundsOrigin);
		QuerySource.bSpatialQuery = true;
		QuerySource.bUseGridLoadingRange = false;
		QuerySource.Radius = static_cast<float>(FMath::Max(BoundsExtent.Size2D(), 1.0));

		const TArray<FWorldPartitionStreamingQuerySource> QuerySources = { QuerySource };
		return Subsystem->IsStreamingCompleted(
			EWorldPartitionRuntimeCellState::Activated,
			QuerySources,
			false);
	}

	return true;
}

void UEMSPhysicsStreamingGuardComponent::SuspendOwnerPhysics()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	SuspendedBodies.Reset();
	for (UActorComponent* Component : Owner->GetComponents())
	{
		UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);

		// The flag is read straight off the body instance rather than through
		// IsSimulatingPhysics, because a Geometry Collection routes that query
		// through its own body instance and answers for the collection as a whole.
		if (!Primitive || !Primitive->BodyInstance.bSimulatePhysics)
		{
			continue;
		}

		FEMSSuspendedBody Body;
		Body.Component = Primitive;
		Body.bWasGravityEnabled = Primitive->BodyInstance.bEnableGravity;

		// Gravity off with no velocity left is all it takes to hold a body in
		// place, and unlike SetSimulatePhysics it changes no physics state.
		//
		// Simulation is deliberately not switched off. On a Geometry Collection
		// that recreates the component's physics state and drops its proxy, and
		// the Geometry Collection actor's restore needs that proxy: it reported
		// "The Chaos physics collection could not be synchronized" for every
		// placed collection in the map while the guard used that lever.
		Primitive->SetAllPhysicsLinearVelocity(FVector::ZeroVector);
		Primitive->SetAllPhysicsAngularVelocityInRadians(FVector::ZeroVector);
		Primitive->SetEnableGravity(false);

		SuspendedBodies.Add(Body);
	}

	bSuspended = !SuspendedBodies.IsEmpty();
	if (bSuspended)
	{
		++StreamingHoldGeneration;
		UE_LOG(
			LogEMSAddonsPhysics,
			Verbose,
			TEXT("EMS physics streaming guard suspended an actor over unloaded world. Actor=%s Bodies=%d"),
			*Owner->GetPathName(),
			SuspendedBodies.Num());
	}
}

void UEMSPhysicsStreamingGuardComponent::ResumeOwnerPhysics()
{
	if (bSuspended) ++StreamingHoldGeneration;
	for (const FEMSSuspendedBody& Body : SuspendedBodies)
	{
		UPrimitiveComponent* Primitive = Body.Component.Get();
		if (!IsValid(Primitive))
		{
			continue;
		}

		Primitive->SetEnableGravity(Body.bWasGravityEnabled);
	}

	if (bSuspended)
	{
		UE_LOG(
			LogEMSAddonsPhysics,
			Verbose,
			TEXT("EMS physics streaming guard released an actor. Actor=%s Bodies=%d"),
			GetOwner() ? *GetOwner()->GetPathName() : TEXT("<none>"),
			SuspendedBodies.Num());
	}

	SuspendedBodies.Reset();
	bSuspended = false;
}

bool UEMSPhysicsStreamingGuardComponent::GetGravityBeforeStreamingHold(
	const UPrimitiveComponent* Component, bool& OutGravity) const
{
	for (const FEMSSuspendedBody& Body : SuspendedBodies)
	{
		if (Body.Component.Get() == Component)
		{
			OutGravity = Body.bWasGravityEnabled;
			return true;
		}
	}
	return false;
}
