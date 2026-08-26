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

namespace
{
	/**
	 * Both probes start slightly inside the owner's bounds.
	 *
	 * A body resting on a floor overlaps it by the solver's contact tolerance, so
	 * a probe starting exactly at the bounds can begin below the surface it is
	 * looking for.
	 */
	constexpr double EMSSupportProbeStartOffset = 2.0;
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

	Super::EndPlay(EndPlayReason);
}

void UEMSPhysicsStreamingGuardComponent::EvaluateInitialSupport()
{
	if (!bSuspended && !HasAnySupportBelow())
	{
		SuspendOwnerPhysics();
	}
}

void UEMSPhysicsStreamingGuardComponent::HandleLevelBeginMakingInvisible(
	UWorld* InWorld,
	const ULevelStreaming* StreamingLevel,
	ULevel* LoadedLevel)
{
	if (InWorld != GetWorld() || bSuspended)
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
	if (InWorld != GetWorld() || !bSuspended)
	{
		return;
	}

	// Any streaming state change at all can be the one that fills the hole, so
	// the probe decides rather than the level's identity or its new state. Nor is
	// LoadedVisible enough on its own: the opening check can suspend an actor a
	// tick before the cell under it finishes becoming visible, and that cell then
	// never reports the edge again. Something loaded below is enough - falling
	// onto it is ordinary physics, not the defect this guards against.
	if (HasAnySupportBelow())
	{
		ResumeOwnerPhysics();
	}
}

bool UEMSPhysicsStreamingGuardComponent::TraceBelowOwner(
	const double Reach,
	FHitResult& OutHit) const
{
	const AActor* Owner = GetOwner();
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World)
	{
		return false;
	}

	FVector BoundsOrigin;
	FVector BoundsExtent;
	Owner->GetActorBounds(true, BoundsOrigin, BoundsExtent, false);

	const FVector Start(
		BoundsOrigin.X,
		BoundsOrigin.Y,
		BoundsOrigin.Z - BoundsExtent.Z + EMSSupportProbeStartOffset);
	const FVector End = Start - FVector(0.0, 0.0, FMath::Max(Reach, 0.0));

	const FCollisionQueryParams QueryParams(
		SCENE_QUERY_STAT(EMSPhysicsStreamingGuard),
		false,
		Owner);
	const FCollisionObjectQueryParams ObjectParams(
		ECC_TO_BITFIELD(ECC_WorldStatic)
		| ECC_TO_BITFIELD(ECC_WorldDynamic)
		| ECC_TO_BITFIELD(ECC_PhysicsBody));

	return World->LineTraceSingleByObjectType(OutHit, Start, End, ObjectParams, QueryParams);
}

bool UEMSPhysicsStreamingGuardComponent::HasAnySupportBelow() const
{
	FHitResult Hit;
	return TraceBelowOwner(MissingWorldProbeDepth, Hit);
}

bool UEMSPhysicsStreamingGuardComponent::IsSupportedByLevel(const ULevel* Level) const
{
	if (!Level)
	{
		return false;
	}

	FHitResult Hit;
	if (!TraceBelowOwner(SupportContactReach, Hit))
	{
		return false;
	}

	const UPrimitiveComponent* HitComponent = Hit.GetComponent();
	const AActor* SupportActor = HitComponent ? HitComponent->GetOwner() : nullptr;
	return SupportActor && SupportActor->GetLevel() == Level;
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
