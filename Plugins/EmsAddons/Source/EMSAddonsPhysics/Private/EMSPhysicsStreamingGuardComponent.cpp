//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSPhysicsStreamingGuardComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "EMSAddonsPhysics.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "LevelUtils.h"
#include "Navigation/PathFollowingComponent.h"
#include "Streaming/LevelStreamingDelegates.h"
#include "TimerManager.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionLevelStreamingDynamic.h"
#include "WorldPartition/WorldPartitionRuntimeCell.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"
#include "WorldPartition/WorldPartitionRuntimeLevelStreamingCell.h"
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

	/**
	 * The same for a character's capsule, whose footprint is a circle: the outer
	 * columns stay inside it rather than at the corners of its bounding box.
	 */
	constexpr double EMSCapsuleProbeFootprintFraction = 0.8 * UE_INV_SQRT_2;

	/** Whether a level is actually on its way to becoming visible. */
	bool IsLevelRequestedForVisibility(const ULevelStreaming* Streaming)
	{
		return Streaming
			&& Streaming->GetLevelStreamingState() != ELevelStreamingState::FailedToLoad
			&& Streaming->ShouldBeLoaded()
			&& Streaming->ShouldBeVisible();
	}

	/** Every Character Movement component on the owner, normally exactly one. */
	void GetCharacterMovements(const AActor* Owner, TArray<UCharacterMovementComponent*, TInlineAllocator<1>>& OutMovements)
	{
		OutMovements.Reset();
		if (!Owner)
		{
			return;
		}
		for (UActorComponent* Component : Owner->GetComponents())
		{
			if (UCharacterMovementComponent* Movement = Cast<UCharacterMovementComponent>(Component))
			{
				OutMovements.Add(Movement);
			}
		}
	}
}

UEMSPhysicsStreamingGuardComponent::UEMSPhysicsStreamingGuardComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	bAutoActivate = true;
}

void UEMSPhysicsStreamingGuardComponent::TickComponent(
	float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const AActor* Owner = GetOwner();
	if (Owner && Owner->GetRootComponent() != ObservedRoot.Get())
	{
		// SetRootComponent has no change notification. Treat a replacement as an
		// arrival: it may already have been moved before we could bind to it.
		ObserveRoot();
		ResetHoldForRelocation();
		EvaluateAfterRelocation();
	}
}

void UEMSPhysicsStreamingGuardComponent::BeginPlay()
{
	Super::BeginPlay();

	// A sublevel that is hidden and shown again routes End Play and then Begin
	// Play on the same component, so nothing may carry over from the last time.
	bSuspended = false;
	SuspendedBodies.Reset();
	SuspendedMovements.Reset();
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
	ObserveRoot();

	if (IsStreamingWorld())
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
	StopObservingRoot();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(InitialEvaluationTimer);
		World->GetTimerManager().ClearTimer(RelocationEvaluationTimer);
		World->GetTimerManager().ClearTimer(DeferredMovementHoldTimer);
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
	if (!bSuspended && !IsArrivalSupported(EEMSHoldReason::Arrival))
	{
		SuspendForReason(EEMSHoldReason::Arrival);
	}
}

bool UEMSPhysicsStreamingGuardComponent::IsArrivalSupported(const EEMSHoldReason Reason) const
{
	// Loaded contact is enough without a partition-wide streaming query. This
	// also keeps a character standing safely at a ledge from being held merely
	// because some unrelated sublevel is hidden.
	if (HasRestingSupport() && HasAnySupportBelow())
	{
		return true;
	}

	// Refresh the loading answer even when there is distant ground. An arriving
	// actor can otherwise fall past an unloaded upper floor onto that ground.
	if (IsWorldBelowOwnerLoaded())
	{
		return true;
	}

	// A departure can fall onto other loaded ground. An arrival must wait for
	// the missing world or actual contact, including in conventional sublevels.
	if (Reason != EEMSHoldReason::Arrival)
	{
		return HasAnySupportBelow();
	}

	// Except where nothing is on its way. Conventional sublevels carry no
	// location, so a hidden sublevel that is not even requested says nothing
	// about the space under an actor dropped or teleported into the air - in a
	// game that keeps sublevels unloaded, every midair spawn would hang for the
	// whole grace period. Loaded ground below catches it as it would anywhere.
	// World Partition keeps the grace period: a cell's request can trail a
	// teleport by a frame, and its answer is spatial. Once the partition itself
	// is loaded below, only such sublevels are missing and the same reasoning
	// applies in a world that mixes both.
	return bWaitingForUnrequestedContent
		&& (!IsWorldPartitionStreaming() || bPartitionLoadedBelow)
		&& HasAnySupportBelow();
}

void UEMSPhysicsStreamingGuardComponent::ReevaluateStreamingSupport()
{
	ResetHoldForRelocation();
	EvaluateAfterRelocation();
}

bool UEMSPhysicsStreamingGuardComponent::IsWorldPartitionStreaming() const
{
	const UWorld* World = GetWorld();
	const UWorldPartition* WorldPartition = World ? World->GetWorldPartition() : nullptr;
	return WorldPartition && WorldPartition->bEnableStreaming;
}

bool UEMSPhysicsStreamingGuardComponent::IsStreamingWorld() const
{
	const UWorld* World = GetWorld();
	return World && (IsWorldPartitionStreaming() || !World->GetStreamingLevels().IsEmpty());
}

void UEMSPhysicsStreamingGuardComponent::ObserveRoot()
{
	StopObservingRoot();
	const AActor* Owner = GetOwner();
	USceneComponent* Root = Owner ? Owner->GetRootComponent() : nullptr;
	if (!Root)
	{
		return;
	}
	ObservedRoot = Root;
	LastObservedLocation = Root->GetComponentLocation();
	RootTransformHandle = Root->TransformUpdated.AddUObject(
		this, &UEMSPhysicsStreamingGuardComponent::HandleRootTransformUpdated);
}

void UEMSPhysicsStreamingGuardComponent::StopObservingRoot()
{
	if (USceneComponent* Root = ObservedRoot.Get())
	{
		Root->TransformUpdated.Remove(RootTransformHandle);
	}
	ObservedRoot.Reset();
	RootTransformHandle.Reset();
}

void UEMSPhysicsStreamingGuardComponent::HandleRootTransformUpdated(
	USceneComponent* UpdatedComponent,
	const EUpdateTransformFlags UpdateTransformFlags,
	const ETeleportType Teleport)
{
	// This runs on every move of the root, so it only compares two locations.
	if (!UpdatedComponent)
	{
		return;
	}
	const FVector Location = UpdatedComponent->GetComponentLocation();
	const double MovedSquared = FVector::DistSquared(Location, LastObservedLocation);
	LastObservedLocation = Location;

	// A simulating body's per-frame sync from physics is not a teleport, whatever
	// it reports. Only an explicit relocation - EMS restoring a saved transform,
	// a checkpoint placing the player, gameplay teleporting - or a jump far
	// beyond any per-frame movement can land the owner over unloaded world.
	const bool bPhysicsSync = EnumHasAnyFlags(UpdateTransformFlags, EUpdateTransformFlags::SkipPhysicsUpdate);
	const bool bTeleported = Teleport != ETeleportType::None
		&& !bPhysicsSync
		&& MovedSquared >= FMath::Square(MinimumTeleportDistance);
	UWorld* World = GetWorld();
	if (!bTeleported)
	{
		// A long step that the owner's own speed explains is movement, not a
		// relocation: a fast fall or a fast body across one long, hitching frame.
		// Twice the step that speed covers in a frame leaves room for the frame
		// time and the speed to disagree a little.
		const AActor* Owner = GetOwner();
		const double ExplainedStep = Owner && World
			? Owner->GetVelocity().Size() * World->GetDeltaSeconds() * 2.0
			: 0.0;
		if (MovedSquared < FMath::Square(RelocationDistance + ExplainedStep))
		{
			return;
		}
	}

	// A held owner also becomes an arrival at the new location. Wait a tick:
	// TeleportTo can change a character's mode after this transform callback,
	// and a restore may still be applying other state to the owner.
	ResetHoldForRelocation();
	if (!World || World->GetTimerManager().TimerExists(RelocationEvaluationTimer))
	{
		return;
	}
	RelocationEvaluationTimer = World->GetTimerManager().SetTimerForNextTick(
		this,
		&UEMSPhysicsStreamingGuardComponent::EvaluateAfterRelocation);
}

void UEMSPhysicsStreamingGuardComponent::ResetHoldForRelocation()
{
	HoldReason = EEMSHoldReason::Arrival;
	UnrequestedSince = -1.0;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DeferredMovementHoldTimer);
	}
}

bool UEMSPhysicsStreamingGuardComponent::IsRelocationEvaluationPending() const
{
	const UWorld* World = GetWorld();
	return World && World->GetTimerManager().TimerExists(RelocationEvaluationTimer);
}

void UEMSPhysicsStreamingGuardComponent::EvaluateAfterRelocation()
{
	// A departure broadcast may have arrived between the transform and this check.
	ResetHoldForRelocation();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RelocationEvaluationTimer);
	}
	if (!IsStreamingWorld())
	{
		ResumeOwnerPhysics();
		return;
	}
	if (bSuspended)
	{
		ForgetExternallyReleasedMovement();
	}
	EvaluateInitialSupport();
	if (bSuspended)
	{
		// TeleportTo may have put a held character back into Falling. Existing
		// MOVE_None holds are left intact; only movement needing a new hold is borrowed.
		HoldCharacterMovements();
		EvaluateSuspendedSupport();
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
	if (!IsSupportedByLevel(LoadedLevel))
	{
		return;
	}

	// Bodies are held at once. A character is held one tick later: switching its
	// movement mode clears its movement base, and the World Partition placed and
	// runtime actor subsystems read that base from this same broadcast to decide
	// which cell the character belongs to and must go dormant with.
	SuspendForReason(EEMSHoldReason::Departure, false);
	TArray<UCharacterMovementComponent*, TInlineAllocator<1>> Movements;
	GetCharacterMovements(GetOwner(), Movements);
	UWorld* World = GetWorld();
	if (World && Movements.ContainsByPredicate(&UEMSPhysicsStreamingGuardComponent::CanHoldCharacterMovement))
	{
		DeferredMovementHoldTimer = World->GetTimerManager().SetTimerForNextTick(
			this,
			&UEMSPhysicsStreamingGuardComponent::HoldMovementAfterDeparture);
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
	EvaluateSuspendedSupport();
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
	struct FEMSProbedBody
	{
		FBox Box;
		double FootprintFraction = EMSSupportProbeFootprintFraction;
		bool bCharacterCapsule = false;
		const UPrimitiveComponent* Primitive = nullptr;
	};
	TArray<FEMSProbedBody, TInlineAllocator<4>> BodyBounds;
	for (const UActorComponent* Component : Owner->GetComponents())
	{
		const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
		if (Primitive && Primitive->IsRegistered() && Primitive->BodyInstance.bSimulatePhysics)
		{
			BodyBounds.Add({ Primitive->Bounds.GetBox(), EMSSupportProbeFootprintFraction, false, Primitive });
		}
	}

	// A character stands on its capsule. The actor's bounds also span its mesh,
	// weapons and attachments, which say nothing about where its feet are.
	TArray<UCharacterMovementComponent*, TInlineAllocator<1>> Movements;
	GetCharacterMovements(Owner, Movements);
	for (const UCharacterMovementComponent* Movement : Movements)
	{
		const UPrimitiveComponent* Capsule = Movement->UpdatedPrimitive;
		if (Capsule && Capsule->IsRegistered() && !Capsule->BodyInstance.bSimulatePhysics)
		{
			BodyBounds.Add({ Capsule->Bounds.GetBox(), EMSCapsuleProbeFootprintFraction, true, Capsule });
		}
	}

	if (BodyBounds.IsEmpty())
	{
		FVector BoundsOrigin;
		FVector BoundsExtent;
		Owner->GetActorBounds(true, BoundsOrigin, BoundsExtent, false);
		BodyBounds.Add({ FBox(BoundsOrigin - BoundsExtent, BoundsOrigin + BoundsExtent), EMSSupportProbeFootprintFraction, false, nullptr });
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

	for (int32 BodyIndex = 0; BodyIndex < BodyBounds.Num(); ++BodyIndex)
	{
		const FEMSProbedBody& Body = BodyBounds[BodyIndex];
		const FBox& Box = Body.Box;
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
				Center.X + Offset.X * Extent.X * Body.FootprintFraction,
				Center.Y + Offset.Y * Extent.Y * Body.FootprintFraction,
				StartZ);
			Column.ReachOriginZ = ReachOriginZ;
			Column.BodyIndex = BodyIndex;
			Column.Body = Body.Primitive;
			Column.bCharacterCapsule = Body.bCharacterCapsule;
		}
	}
}

bool UEMSPhysicsStreamingGuardComponent::IsCharacterStandingOnLoadedFloor() const
{
	// Character Movement has already found the floor under the whole capsule,
	// edges and ledges included, which a handful of probe lines cannot match. A
	// floor that belongs to a departing level does not count.
	TArray<UCharacterMovementComponent*, TInlineAllocator<1>> Movements;
	GetCharacterMovements(GetOwner(), Movements);
	if (Movements.IsEmpty())
	{
		return false;
	}
	for (const UCharacterMovementComponent* Movement : Movements)
	{
		if (!Movement->UpdatedComponent)
		{
			return false;
		}
		FFindFloorResult FreshFloor;
		const FFindFloorResult* FloorResult = &Movement->CurrentFloor;
		if (!Movement->IsMovingOnGround())
		{
			// MOVE_None clears CurrentFloor. Recheck the capsule while held so a
			// newly returned floor can release a character standing at a ledge.
			const bool bHeldByGuard = SuspendedMovements.ContainsByPredicate(
				[Movement](const FEMSSuspendedMovement& Held) { return Held.Movement.Get() == Movement; });
			if (!bHeldByGuard || Movement->MovementMode != MOVE_None)
			{
				return false;
			}
			Movement->FindFloor(Movement->UpdatedComponent->GetComponentLocation(), FreshFloor, false);
			FloorResult = &FreshFloor;
		}
		else if (IsCachedFloorStale(*Movement))
		{
			// CurrentFloor is only refreshed when Character Movement runs. A walking
			// character moved by SetActorLocation - an EMS restore, a checkpoint -
			// keeps the floor of where it stood until then, and on a server a remote
			// player's movement only runs when its next move arrives. That old floor
			// says nothing about the destination, so sweep the capsule where it is.
			Movement->FindFloor(Movement->UpdatedComponent->GetComponentLocation(), FreshFloor, false);
			FloorResult = &FreshFloor;
		}

		const UPrimitiveComponent* Floor = FloorResult->HitResult.GetComponent();
		const ULevel* FloorLevel = Floor ? Floor->GetComponentLevel() : nullptr;
		if (!FloorResult->IsWalkableFloor()
			|| FloorResult->GetDistanceToFloor() > SupportContactReach
			|| !Floor
			|| (FloorLevel && (FloorLevel->bIsBeingRemoved || DepartingLevels.Contains(FloorLevel))))
		{
			return false;
		}
	}
	return true;
}

bool UEMSPhysicsStreamingGuardComponent::IsCachedFloorStale(const UCharacterMovementComponent& Movement) const
{
	// A cleared floor is Character Movement's own answer, not a stale one.
	const FHitResult& Hit = Movement.CurrentFloor.HitResult;
	const UPrimitiveComponent* Floor = Hit.GetComponent();
	if (!Floor || !Movement.CurrentFloor.bBlockingHit)
	{
		return false;
	}

	// A floor that has since lost its collision - unregistered with a level that
	// finished streaming out, or switched off - no longer holds anything up.
	if (!Floor->IsRegistered() || !Floor->IsQueryCollisionEnabled())
	{
		return true;
	}

	// The floor's contact must lie under the capsule as it is now: inside its
	// footprint, and between its centre and the contact probe's reach below it.
	const FBox Capsule = Movement.UpdatedComponent->Bounds.GetBox();
	if (!Capsule.IsValid)
	{
		return true;
	}
	constexpr double Tolerance = 5.0;
	const FVector Center = Capsule.GetCenter();
	const FVector Extent = Capsule.GetExtent();
	const double Radius = FMath::Min(Extent.X, Extent.Y) + Tolerance;
	const FVector& Impact = Hit.ImpactPoint;
	return FVector::DistSquared2D(Impact, Center) > FMath::Square(Radius)
		|| Impact.Z > Center.Z
		|| Impact.Z < Capsule.Min.Z - SupportContactReach - Tolerance;
}

bool UEMSPhysicsStreamingGuardComponent::HasRestingSupport() const
{
	TArray<FEMSSupportProbeColumn> Columns;
	GatherSupportProbeColumns(Columns);
	if (Columns.IsEmpty())
	{
		return false;
	}

	// Resting means touching loaded collision now, not merely having some far
	// below: that is what tells a character standing on a floor from one that
	// was teleported into the air above a floor still streaming in.
	const bool bStanding = IsCharacterStandingOnLoadedFloor();
	TArray<UCharacterMovementComponent*, TInlineAllocator<1>> Movements;
	GetCharacterMovements(GetOwner(), Movements);
	int32 CurrentBodyIndex = INDEX_NONE;
	bool bBodyRests = false;
	for (const FEMSSupportProbeColumn& Column : Columns)
	{
		if (Column.BodyIndex != CurrentBodyIndex)
		{
			if (CurrentBodyIndex != INDEX_NONE && !bBodyRests)
			{
				return false;
			}
			CurrentBodyIndex = Column.BodyIndex;
			bBodyRests = Column.bCharacterCapsule && bStanding;
		}
		if (!bBodyRests)
		{
			FHitResult Hit;
			if (TraceSupportColumn(Column, SupportContactReach, Hit, true))
			{
				if (!Column.bCharacterCapsule)
				{
					bBodyRests = true;
				}
				else
				{
					// A blocking surface is not necessarily a floor a character can
					// stand on: respect slope limits and component overrides as well.
					for (const UCharacterMovementComponent* Movement : Movements)
					{
						if (Movement->UpdatedPrimitive == Column.Body && Movement->IsWalkable(Hit))
						{
							bBodyRests = true;
							break;
						}
					}
				}
			}
		}
	}
	return CurrentBodyIndex != INDEX_NONE && bBodyRests;
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
	// Supporting geometry may use Vehicle, Destructible or a project's custom
	// object channel. The mutual blocking response below decides what is ground.
	const FCollisionObjectQueryParams ObjectParams(FCollisionObjectQueryParams::AllObjects);

	// An object query reports everything whatever its collision
	// responses are, so a trigger, a physics volume or any other overlap-only
	// shape would answer as ground. Every hit is looked at and only one the body
	// actually collides with counts. Looking past the first hit also finds a
	// loaded floor below a trigger or below a departing floor.
	TArray<FHitResult> Hits;
	World->LineTraceMultiByObjectType(Hits, Start, End, ObjectParams, QueryParams);
	for (const FHitResult& Hit : Hits)
	{
		UPrimitiveComponent* SupportComponent = Hit.GetComponent();
		if (SupportComponent && Column.Body)
		{
			if (Column.Body->GetCollisionResponseToComponent(SupportComponent) != ECR_Block)
			{
				continue;
			}

			// Character Movement stands on query collision; a simulating body needs
			// the floor in the physics scene.
			if (!Column.bCharacterCapsule && !CollisionEnabledHasPhysics(SupportComponent->GetCollisionEnabled()))
			{
				continue;
			}
		}

		// BSP collision has no owning actor; the component level is its outer level.
		// A hit whose level cannot be named at all cannot be departing either.
		ULevel* SupportLevel = SupportComponent ? SupportComponent->GetComponentLevel() : nullptr;
		if (bIgnoreDepartingLevels
			&& SupportLevel
			&& (SupportLevel->bIsBeingRemoved || DepartingLevels.Contains(SupportLevel)))
		{
			continue;
		}
		OutHit = Hit;
		return true;
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
	// it then would tip it into the gap the guard exists to bridge. A character
	// standing on a loaded floor is supported however far its capsule overhangs.
	const bool bStanding = IsCharacterStandingOnLoadedFloor();
	for (const FEMSSupportProbeColumn& Column : Columns)
	{
		if (Column.bCharacterCapsule && bStanding)
		{
			continue;
		}
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

	// A character knows what it stands on: its movement base, including a lift or
	// a vehicle that the probe below its capsule might not reach.
	TArray<UCharacterMovementComponent*, TInlineAllocator<1>> Movements;
	GetCharacterMovements(GetOwner(), Movements);
	for (const UCharacterMovementComponent* Movement : Movements)
	{
		const UPrimitiveComponent* Base = Cast<UPrimitiveComponent>(Movement->GetMovementBaseObject());
		if (Base && Base->GetComponentLevel() == Level)
		{
			return true;
		}
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

	// Physics-only shapes are absent from the world's query acceleration
	// structure. At this edge we know the departing level, so inspect only its
	// physics-only components and trace their own physics geometry directly.
	// Only simulating bodies can rest on them: Character Movement stands on
	// query collision. Every guarded AI character answers this broadcast, so it
	// must not walk the departing level's actors for nothing.
	const bool bHasSimulatingColumn = Columns.ContainsByPredicate([](const FEMSSupportProbeColumn& Column)
	{
		return Column.Body && !Column.bCharacterCapsule;
	});
	if (!bHasSimulatingColumn)
	{
		return false;
	}
	for (const AActor* Actor : Level->Actors)
	{
		if (!IsValid(Actor) || Actor == GetOwner()) continue;
		for (UActorComponent* Component : Actor->GetComponents())
		{
			UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
			if (!Primitive || !Primitive->IsRegistered()
				|| Primitive->GetCollisionEnabled() != ECollisionEnabled::PhysicsOnly) continue;
			const FBox Bounds = Primitive->Bounds.GetBox();
			for (const FEMSSupportProbeColumn& Column : Columns)
			{
				if (!Column.Body || Column.bCharacterCapsule
					|| Column.Body->GetCollisionResponseToComponent(Primitive) != ECR_Block) continue;
				const FVector End(Column.Start.X, Column.Start.Y, Column.ReachOriginZ - FMath::Max(SupportContactReach, 0.0));
				if (!FMath::LineBoxIntersection(Bounds, Column.Start, End, End - Column.Start)) continue;
				FHitResult Hit;
				if (Primitive->LineTraceComponent(Hit, Column.Start, End, FCollisionQueryParams::DefaultQueryParam)) return true;
			}
		}
	}
	return false;
}

bool UEMSPhysicsStreamingGuardComponent::IsWorldBelowOwnerLoaded() const
{
	bWaitingForUnrequestedContent = false;
	bPartitionLoadedBelow = false;
	const AActor* Owner = GetOwner();
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World)
	{
		return true;
	}

	// A level still on its way out has not finished changing what lies below.
	// A World Partition cell is the exception: it stops counting as active the
	// moment it starts to leave, so the spatial query below answers for it, and
	// only where it lies under the owner. Otherwise the cells streaming out far
	// behind a teleported player would hold it in the air over a loaded floor.
	const bool bCellsAnsweredSpatially = IsWorldPartitionStreaming();
	bool bDepartingSublevel = false;
	for (const TWeakObjectPtr<ULevel>& Departing : DepartingLevels)
	{
		const ULevel* DepartingLevel = Departing.Get();
		if (!DepartingLevel)
		{
			continue;
		}
		if (bCellsAnsweredSpatially
			&& Cast<UWorldPartitionLevelStreamingDynamic>(FLevelUtils::FindStreamingLevel(DepartingLevel)))
		{
			continue;
		}
		bDepartingSublevel = true;
	}

	// Conventional sublevels and unembedded level instances have no location the
	// guard could test against, so any of them that is not fully visible might be
	// the missing floor. World Partition cells are answered spatially below.
	bool bMissingSublevel = bDepartingSublevel;
	bool bRequestedSublevel = bDepartingSublevel;
	for (const ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
	{
		if (!StreamingLevel || StreamingLevel->IsA<UWorldPartitionLevelStreamingDynamic>())
		{
			continue;
		}
		if (StreamingLevel->GetLevelStreamingState() != ELevelStreamingState::LoadedVisible)
		{
			bMissingSublevel = true;
			bRequestedSublevel |= IsLevelRequestedForVisibility(StreamingLevel);
		}
	}
	// Neither source may hide a request from the other. An unrelated hidden
	// sublevel must not start the expiry timer while the destination cell loads.
	bool bRequestedPartition = false;
	const bool bPartitionLoaded = !bCellsAnsweredSpatially || IsWorldPartitionBelowOwnerLoaded(bRequestedPartition);
	const bool bLoaded = !bMissingSublevel && bPartitionLoaded;
	bPartitionLoadedBelow = bCellsAnsweredSpatially && bPartitionLoaded;
	bWaitingForUnrequestedContent = !bLoaded && !bRequestedSublevel && !bRequestedPartition;
	return bLoaded;
}

bool UEMSPhysicsStreamingGuardComponent::IsWorldPartitionBelowOwnerLoaded(bool& bOutRequested) const
{
	bOutRequested = false;
	const AActor* Owner = GetOwner();
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World)
	{
		return true;
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
		const bool bLocalCellsReady = Subsystem->IsStreamingCompleted(
			EWorldPartitionRuntimeCellState::Activated,
			QuerySources,
			false);

		// The query only covers the grid cells at the owner's location. An actor
		// is assigned to one cell by its position, so a large floor can reach far
		// beyond its own cell, under a spot whose own cells are all active. Any
		// cell that is still not active and whose content lies under the owner's
		// footprint may hold the missing floor.
		bool bPendingContent = false;
		bool bRequestedContent = false;
		InspectWorldPartitionContentBelow(
			*WorldPartition,
			FBox(BoundsOrigin - BoundsExtent, BoundsOrigin + BoundsExtent),
			bPendingContent,
			bRequestedContent);
		const bool bLoaded = bLocalCellsReady && !bPendingContent;
		bOutRequested = bRequestedContent;
		return bLoaded;
	}

	return true;
}

void UEMSPhysicsStreamingGuardComponent::InspectWorldPartitionContentBelow(
	const UWorldPartition& WorldPartition,
	const FBox& OwnerBounds,
	bool& bOutPending,
	bool& bOutRequested) const
{
	bOutPending = false;
	bOutRequested = false;
	const UWorld* World = GetWorld();
	if (!World || !WorldPartition.RuntimeHash || !OwnerBounds.IsValid)
	{
		return;
	}

	// Cell content is stored in the partition's own space.
	const FBox LocalOwner = OwnerBounds.TransformBy(WorldPartition.GetInstanceTransform().Inverse());
	const bool bDedicatedServer = World->GetNetMode() == NM_DedicatedServer;
	const bool bServerLoadsAll = WorldPartition.IsServer() && !WorldPartition.IsServerStreamingEnabled();
	const FWorldPartitionStreamingContext StreamingContext = FWorldPartitionStreamingContext::Create(World);
	WorldPartition.RuntimeHash->ForEachStreamingCells([&](const UWorldPartitionRuntimeCell* Cell)
	{
		// HLOD is visual only. The other skips mirror the engine's own streaming
		// completion: cells this machine never activates, and cells whose data
		// layers are not meant to be active, are not on their way in.
		if (!Cell
			|| Cell->GetIsHLOD()
			|| Cell->GetIsCustomHLODPlaceholderCell()
			|| (bDedicatedServer && Cell->GetClientOnlyVisible())
			|| Cell->GetCurrentState() == EWorldPartitionRuntimeCellState::Activated
			|| (Cell->HasDataLayers() && Cell->GetCellEffectiveWantedState(StreamingContext) != EDataLayerRuntimeState::Activated))
		{
			return true;
		}

		const FBox& Content = Cell->GetContentBounds();
		if (Content.IsValid
			&& Content.Min.X <= LocalOwner.Max.X && Content.Max.X >= LocalOwner.Min.X
			&& Content.Min.Y <= LocalOwner.Max.Y && Content.Max.Y >= LocalOwner.Min.Y
			&& Content.Min.Z <= LocalOwner.Max.Z)
		{
			bOutPending = true;
			const UWorldPartitionRuntimeLevelStreamingCell* LevelCell = Cast<UWorldPartitionRuntimeLevelStreamingCell>(Cell);
			const ULevelStreaming* Streaming = LevelCell ? LevelCell->GetLevelStreaming() : nullptr;
			bOutRequested |= IsLevelRequestedForVisibility(Streaming) || (bServerLoadsAll && !Streaming);
			if (bOutRequested)
			{
				return false;
			}
		}
		return true;
	});
}

bool UEMSPhysicsStreamingGuardComponent::CanHoldCharacterMovement(const UCharacterMovementComponent* Movement)
{
	if (!IsValid(Movement) || !Movement->IsActive() || !Movement->UpdatedComponent)
	{
		return false;
	}

	// Only the machine whose movement is authoritative may hold it. A client that
	// switched its own character to None would send that mode to the server and be
	// corrected straight back; the server's hold reaches the client the same way.
	const AActor* Owner = Movement->GetOwner();
	if (!Owner || Owner->GetLocalRole() != ROLE_Authority)
	{
		return false;
	}

	// Only movement that gravity pulls down. A flying, swimming or custom-moving
	// character does not fall through anything, and one already set to None is
	// held by whoever set it.
	switch (Movement->MovementMode)
	{
	case MOVE_Walking:
	case MOVE_NavWalking:
	case MOVE_Falling:
		return true;
	default:
		return false;
	}
}

void UEMSPhysicsStreamingGuardComponent::SuspendOwnerPhysics()
{
	SuspendForReason(EEMSHoldReason::Departure);
}

void UEMSPhysicsStreamingGuardComponent::HoldCharacterMovements()
{
	TArray<UCharacterMovementComponent*, TInlineAllocator<1>> Movements;
	GetCharacterMovements(GetOwner(), Movements);
	for (UCharacterMovementComponent* Movement : Movements)
	{
		if (!CanHoldCharacterMovement(Movement))
		{
			continue;
		}

		// Character Movement applies its own gravity, so the physics lever does
		// nothing for it. Movement mode None stops it simulating altogether -
		// no gravity, no input, no jumping - without touching its collision.
		FEMSSuspendedMovement Held;
		Held.Movement = Movement;
		Held.MovementMode = Movement->MovementMode;
		Held.CustomMovementMode = Movement->CustomMovementMode;

		// An AI's Move To survives the hold. StopMovementImmediately would abort
		// it, and a request left running would be failed by block detection once
		// the character stops making progress, so it is paused and resumed.
		const APawn* Pawn = Cast<APawn>(GetOwner());
		const AController* Controller = Pawn ? Pawn->GetController() : nullptr;
		UPathFollowingComponent* PathFollowing = Controller ? Controller->FindComponentByClass<UPathFollowingComponent>() : nullptr;
		if (PathFollowing && PathFollowing->GetStatus() == EPathFollowingStatus::Moving)
		{
			Held.PathFollowing = PathFollowing;
			Held.PathRequestId = PathFollowing->GetCurrentRequestId().GetID();
			PathFollowing->PauseMove(PathFollowing->GetCurrentRequestId());
		}

		// Switching to None already clears velocity and pending forces while
		// keeping any path, which is exactly what StopMovementKeepPathing does.
		Movement->SetMovementMode(MOVE_None);
		Movement->Velocity = FVector::ZeroVector;
		SuspendedMovements.Add(Held);
	}
}

void UEMSPhysicsStreamingGuardComponent::ResumePathFollowing(const FEMSSuspendedMovement& Held)
{
	UPathFollowingComponent* PathFollowing = Held.PathFollowing.Get();
	if (IsValid(PathFollowing)
		&& PathFollowing->GetStatus() == EPathFollowingStatus::Paused
		&& PathFollowing->GetCurrentRequestId().GetID() == Held.PathRequestId)
	{
		PathFollowing->ResumeMove(PathFollowing->GetCurrentRequestId());
	}
}

void UEMSPhysicsStreamingGuardComponent::HoldMovementAfterDeparture()
{
	if (!SuspendedMovements.IsEmpty())
	{
		return;
	}

	// One tick has passed since the floor announced its departure. Collision of a
	// departing level no longer counts, so loaded support below means another
	// floor holds the character and there is nothing to bridge.
	TArray<UCharacterMovementComponent*, TInlineAllocator<1>> Movements;
	GetCharacterMovements(GetOwner(), Movements);
	if (!Movements.ContainsByPredicate(&UEMSPhysicsStreamingGuardComponent::CanHoldCharacterMovement)
		|| HasAnySupportBelow())
	{
		return;
	}

	HoldCharacterMovements();
	if (!bSuspended && !SuspendedMovements.IsEmpty())
	{
		BeginHold(EEMSHoldReason::Departure);
	}
}

void UEMSPhysicsStreamingGuardComponent::SuspendForReason(const EEMSHoldReason Reason, const bool bHoldMovement)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	SuspendedBodies.Reset();
	SuspendedMovements.Reset();
	if (bHoldMovement)
	{
		HoldCharacterMovements();
	}
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

	bSuspended = false;
	if (!SuspendedBodies.IsEmpty() || !SuspendedMovements.IsEmpty())
	{
		BeginHold(Reason);
	}
}

void UEMSPhysicsStreamingGuardComponent::BeginHold(const EEMSHoldReason Reason)
{
	bSuspended = true;
	++StreamingHoldGeneration;
	HoldReason = Reason;
	UWorld* World = GetWorld();
	UnrequestedSince = -1.0;
	if (World)
	{
		World->GetTimerManager().SetTimer(SuspendedEvaluationTimer, this,
			&UEMSPhysicsStreamingGuardComponent::EvaluateSuspendedSupport, 0.1f, true);
	}
	UE_LOG(
		LogEMSAddonsPhysics,
		Verbose,
		TEXT("EMS physics streaming guard held an actor over unloaded world. Actor=%s Bodies=%d Characters=%d Reason=%s"),
		GetOwner() ? *GetOwner()->GetPathName() : TEXT("<none>"),
		SuspendedBodies.Num(),
		SuspendedMovements.Num(),
		Reason == EEMSHoldReason::Arrival ? TEXT("Arrival") : TEXT("Departure"));
}

void UEMSPhysicsStreamingGuardComponent::ResumeOwnerPhysics()
{
	if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(SuspendedEvaluationTimer);
	if (bSuspended) ++StreamingHoldGeneration;
	for (const FEMSSuspendedBody& Body : SuspendedBodies)
	{
		UPrimitiveComponent* Primitive = Body.Component.Get();
		if (!IsValid(Primitive))
		{
			continue;
		}

		// The hold left gravity off. If it is on again, gameplay switched it on while
		// the body was held, and that decision stands over the borrowed value.
		if (!Primitive->BodyInstance.bEnableGravity)
		{
			Primitive->SetEnableGravity(Body.bWasGravityEnabled);
		}
	}

	for (const FEMSSuspendedMovement& Held : SuspendedMovements)
	{
		// A mode other than None means gameplay moved the character on itself
		// while it was held - an ability, a cinematic, a possession change - and
		// that decision stands.
		UCharacterMovementComponent* Movement = Held.Movement.Get();
		if (IsValid(Movement) && Movement->MovementMode == MOVE_None)
		{
			Movement->SetMovementMode(
				static_cast<EMovementMode>(Held.MovementMode),
				Held.CustomMovementMode);
		}
		ResumePathFollowing(Held);
	}

	if (bSuspended)
	{
		UE_LOG(
			LogEMSAddonsPhysics,
			Verbose,
			TEXT("EMS physics streaming guard released an actor. Actor=%s Bodies=%d Characters=%d"),
			GetOwner() ? *GetOwner()->GetPathName() : TEXT("<none>"),
			SuspendedBodies.Num(),
			SuspendedMovements.Num());
	}

	SuspendedBodies.Reset();
	SuspendedMovements.Reset();
	bSuspended = false;
}

void UEMSPhysicsStreamingGuardComponent::ForgetExternallyReleasedMovement()
{
	const int32 Removed = SuspendedMovements.RemoveAll([](const FEMSSuspendedMovement& Held)
	{
		const UCharacterMovementComponent* Movement = Held.Movement.Get();
		if (IsValid(Movement) && Movement->MovementMode == MOVE_None)
		{
			return false;
		}
		// Gameplay ended this hold itself; the paused move belongs to it again.
		ResumePathFollowing(Held);
		return true;
	});
	if (Removed == 0 || !SuspendedMovements.IsEmpty())
	{
		return;
	}
	for (const FEMSSuspendedBody& Body : SuspendedBodies)
	{
		if (IsValid(Body.Component.Get()))
		{
			return;
		}
	}

	// Nothing the guard borrowed is still borrowed, so the hold has ended.
	ResumeOwnerPhysics();
}

void UEMSPhysicsStreamingGuardComponent::EvaluateSuspendedSupport()
{
	if (!bSuspended || IsRelocationEvaluationPending())
	{
		return;
	}
	ForgetExternallyReleasedMovement();
	if (!bSuspended)
	{
		return;
	}

	if (IsArrivalSupported(HoldReason))
	{
		ResumeOwnerPhysics();
		return;
	}

	// A hidden sublevel or cell may never be requested. Give streaming sources
	// time to update, but do not strand either a character or a physics body.
	// A fresh grace period starts whenever requested content stops being requested.
	const UWorld* World = GetWorld();
	if (!World || HoldReason != EEMSHoldReason::Arrival || !bWaitingForUnrequestedContent)
	{
		UnrequestedSince = -1.0;
		return;
	}
	if (UnrequestedSince < 0.0)
	{
		UnrequestedSince = World->GetTimeSeconds();
	}
	if (World->GetTimeSeconds() - UnrequestedSince >= ArrivalHoldLimitWithoutLocation)
	{
		UE_LOG(
			LogEMSAddonsPhysics,
			Log,
			TEXT("EMS physics streaming guard released an actor after %.1f s waiting for unrequested world. Actor=%s"),
			ArrivalHoldLimitWithoutLocation,
			GetOwner() ? *GetOwner()->GetPathName() : TEXT("<none>"));
		ResumeOwnerPhysics();
	}
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
