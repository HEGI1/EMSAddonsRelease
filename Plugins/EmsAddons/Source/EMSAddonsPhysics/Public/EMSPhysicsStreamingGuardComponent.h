//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "EMSPhysicsStreamingGuardComponent.generated.h"

class ULevel;
class ULevelStreaming;
class UPrimitiveComponent;
struct FHitResult;
enum class ELevelStreamingState : uint8;

/**
 * Holds a simulating actor in place while the world underneath it is unloaded.
 *
 * An actor resting on geometry that lives in a streamed level or a World
 * Partition cell keeps falling the moment that level is hidden: the floor's
 * collision is gone, gravity is not. By the time the cell streams back in the
 * actor is far below the world, and a save taken meanwhile records it there.
 *
 * The guard reacts to the two streaming edges. While held, it also checks for
 * movement onto loaded support every 0.1 seconds. It suspends
 * only when the level that is actually holding the owner up is about to become
 * invisible, or when support is missing below the owner at Begin Play while the
 * world there is still streaming in (World Partition streaming or a conventional
 * streamed sublevel) - a static world, or a fully loaded one, has nothing to
 * explain missing support with, so the owner is left to fall. Streaming events
 * resume it when usable support is back below the owner, ignoring departing
 * collision, or when the world below has finished loading. Ordinary gameplay -
 * debris falling into a pit, an object knocked off a ledge, a body spawned over
 * a chasm - never reaches either condition.
 *
 * Add it to any actor with simulating primitive components. It is a gameplay
 * safeguard rather than persistence: it runs in every net mode, it is not
 * server-authoritative, and it never reads or writes save data.
 */
UCLASS(
	ClassGroup = (EasyMultiSave),
	meta = (BlueprintSpawnableComponent, DisplayName = "EMS Physics Streaming Guard"))
class EMSADDONSPHYSICS_API UEMSPhysicsStreamingGuardComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEMSPhysicsStreamingGuardComponent();

	/** True while the guard is holding the owner because the world below it is unloaded. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Physics")
	bool IsPhysicsSuspendedForStreaming() const
	{
		return bSuspended;
	}

	/** Persistence captures the gravity flag borrowed by this guard, not its temporary hold. */
	bool GetGravityBeforeStreamingHold(const UPrimitiveComponent* Component, bool& OutGravity) const;
	/** Changes whenever a streaming hold begins or ends. */
	uint64 GetStreamingHoldGeneration() const { return StreamingHoldGeneration; }

	/**
	 * Reach of the resting-contact probe, in centimeters.
	 *
	 * Internal tuning rather than a designer setting, in the same spirit as the
	 * Geometry Collection actor's retry budget. Long enough to survive physics
	 * settling jitter, short enough not to reach unrelated geometry below a gap.
	 * Left assignable so automation can drive both probes in a small test world.
	 */
	double SupportContactReach = 22.0;

	/**
	 * Depth of the "is any world loaded below" probe, in centimeters.
	 *
	 * Deliberately far longer than the contact probe: this one answers whether
	 * the owner is over an unloaded hole or merely airborne over loaded geometry,
	 * and only the first is the guard's business.
	 */
	double MissingWorldProbeDepth = 100000.0;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * Isolated so automation can exercise the guard without a streaming world.
	 *
	 * Both probes cast several columns across the footprint of every simulating
	 * body rather than one line from the actor's centre, so a wide or overhanging
	 * body is judged by where it actually rests. HasAnySupportBelow needs loaded
	 * collision below every column; IsSupportedByLevel needs one column in contact
	 * with the level.
	 */
	virtual bool HasAnySupportBelow() const;
	virtual bool IsSupportedByLevel(const ULevel* Level) const;

	/**
	 * True when nothing below the owner can still be waiting to stream in.
	 *
	 * Missing support is only the guard's business while the world under the
	 * owner is unloaded. Once it is loaded, missing support is a real void - a
	 * pit, a chasm, the edge of the map - and the owner must fall into it like
	 * any other physics body. World Partition answers this for the owner's
	 * location. Conventional streamed sublevels carry no location, so any of them
	 * that is not fully visible keeps the answer at "not loaded".
	 */
	virtual bool IsWorldBelowOwnerLoaded() const;

	void EvaluateInitialSupport();
	void SuspendOwnerPhysics();
	void ResumeOwnerPhysics();

private:
	/** The borrowed gravity flag of one primitive, so resuming is exact. */
	struct FEMSSuspendedBody
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		bool bWasGravityEnabled = false;
	};

	TArray<FEMSSuspendedBody> SuspendedBodies;
	/** Collision can remain registered after a level announces its departure. */
	TSet<TWeakObjectPtr<ULevel>> DepartingLevels;
	FDelegateHandle BeginInvisibleHandle;
	FDelegateHandle StateChangedHandle;
	FTimerHandle InitialEvaluationTimer;
	FTimerHandle SuspendedEvaluationTimer;
	void EvaluateSuspendedSupport();
	bool bSuspended = false;
	uint64 StreamingHoldGeneration = 0;

	/** One vertical probe line: where it starts and the Z its reach is measured from. */
	struct FEMSSupportProbeColumn
	{
		FVector Start = FVector::ZeroVector;
		double ReachOriginZ = 0.0;
	};

	void GatherSupportProbeColumns(TArray<FEMSSupportProbeColumn>& OutColumns) const;
	bool TraceSupportColumn(
		const FEMSSupportProbeColumn& Column,
		double Reach,
		FHitResult& OutHit,
		bool bIgnoreDepartingLevels) const;

	void HandleLevelBeginMakingInvisible(
		UWorld* InWorld,
		const ULevelStreaming* StreamingLevel,
		ULevel* LoadedLevel);

	void HandleLevelStreamingStateChanged(
		UWorld* InWorld,
		const ULevelStreaming* StreamingLevel,
		ULevel* LoadedLevel,
		ELevelStreamingState PreviousState,
		ELevelStreamingState NewState);
};
