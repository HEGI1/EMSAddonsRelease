//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "EMSPhysicsStreamingGuardComponent.generated.h"

class UCharacterMovementComponent;
class UPathFollowingComponent;
class ULevel;
class ULevelStreaming;
class UPrimitiveComponent;
class USceneComponent;
class UWorldPartition;
struct FHitResult;
enum class ELevelStreamingState : uint8;
enum class ETeleportType : uint8;

/**
 * Holds an actor in place while the world underneath it is unloaded.
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
 * The Begin Play check runs again after the owner is teleported - by an EMS
 * load, a checkpoint restore or gameplay - because a teleport can land it over
 * world that has not streamed in yet.
 *
 * Simulating primitive components are held by switching their gravity off.
 * Characters are held by switching their Character Movement to None, and only
 * where that movement is authoritative (standalone, listen server, dedicated
 * server): a client-side change would only be corrected away by the server.
 * Player pawns and AI characters receive a guard automatically; see the
 * Physics Streaming Guard project settings. It is a gameplay safeguard rather
 * than persistence and never reads or writes save data.
 */
UCLASS(
	ClassGroup = (EasyMultiSave),
	meta = (BlueprintSpawnableComponent, DisplayName = "EMS Physics Streaming Guard"))
class EMSADDONSPHYSICS_API UEMSPhysicsStreamingGuardComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEMSPhysicsStreamingGuardComponent();

	/** True while the guard is holding the owner, physics or character movement, because the world below it is unloaded. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Physics")
	bool IsPhysicsSuspendedForStreaming() const
	{
		return bSuspended;
	}

	/**
	 * Checks again whether the owner is over world that has not streamed in yet.
	 *
	 * Teleports and long jumps in location are detected automatically. Call this
	 * only after moving the owner by means that report neither.
	 */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Physics")
	void ReevaluateStreamingSupport();

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

	/**
	 * A move of the owner's root longer than this in one update counts as a
	 * relocation even without a teleport flag. Far beyond any per-frame movement.
	 */
	double RelocationDistance = 500.0;

	/**
	 * Teleports shorter than this are ignored. A character on a moving platform
	 * is moved with a teleport flag every frame, and a step that short cannot
	 * carry the owner over world that was not already loaded around it.
	 */
	double MinimumTeleportDistance = 100.0;

	/**
	 * Longest hold, in seconds, of an owner that appeared or was teleported over
	 * missing support while nothing can say the missing world will arrive.
	 *
	 * An unloaded cell or sublevel may never be requested. Arrival holds waiting
	 * only for such content expire, while content actually being loaded and holds
	 * that began because a floor streamed out have no limit.
	 */
	double ArrivalHoldLimitWithoutLocation = 10.0;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

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
	 * True when every body touches loaded collision now: a character on a
	 * walkable floor, or a body with a probe column within contact reach.
	 */
	virtual bool HasRestingSupport() const;

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

	/** Spatial partition readiness and whether missing content is requested, combined with sublevels above. */
	virtual bool IsWorldPartitionBelowOwnerLoaded(bool& bOutRequested) const;

	/** Holds the owner when it is over missing support while the world below is still loading. */
	void EvaluateInitialSupport();
	void SuspendOwnerPhysics();
	void ResumeOwnerPhysics();
	/** Runs the periodic check of a held owner: release on support, loaded void or expiry. */
	void EvaluateSuspendedSupport();

	/** Whether this character movement may be held here: authoritative and subject to gravity. */
	static bool CanHoldCharacterMovement(const UCharacterMovementComponent* Movement);

	/**
	 * True while World Partition streaming or any streamed sublevel can leave
	 * the world below the owner unloaded. Read live, because level instances and
	 * dynamically loaded sublevels can turn a static world into a streaming one.
	 */
	virtual bool IsStreamingWorld() const;

	/** The check scheduled one tick after a teleport or a long jump in location. */
	void EvaluateAfterRelocation();
	bool IsRelocationEvaluationPending() const;

	/** The character half of a departure hold, one tick after the floor began to leave. */
	void HoldMovementAfterDeparture();

	/** True when World Partition streams this world, so "the world below has loaded" has a spatial answer. */
	virtual bool IsWorldPartitionStreaming() const;

	/**
	 * Recorded by IsWorldBelowOwnerLoaded: missing content is not currently
	 * requested to become visible, so it might never arrive.
	 */
	void SetWaitingForUnrequestedContent(const bool bWaiting) const
	{
		bWaitingForUnrequestedContent = bWaiting;
		bPartitionLoadedBelow = false;
	}

private:
	/** Why the current hold began, which decides how it may end. */
	enum class EEMSHoldReason : uint8
	{
		/** The level holding the owner up began to leave. */
		Departure,
		/** The owner appeared or was teleported over world still streaming in. */
		Arrival,
	};

	/** The borrowed gravity flag of one primitive, so resuming is exact. */
	struct FEMSSuspendedBody
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		bool bWasGravityEnabled = false;
	};

	/** The borrowed movement mode of one character, so resuming is exact. */
	struct FEMSSuspendedMovement
	{
		TWeakObjectPtr<UCharacterMovementComponent> Movement;
		uint8 MovementMode = 0;
		uint8 CustomMovementMode = 0;
		/** The AI Move To paused for the hold, resumed with it. */
		TWeakObjectPtr<UPathFollowingComponent> PathFollowing;
		uint32 PathRequestId = 0;
	};

	static void ResumePathFollowing(const FEMSSuspendedMovement& Held);
	/** Whether the owner needs no hold, judged by why it would be held. */
	bool IsArrivalSupported(EEMSHoldReason Reason) const;
	/** True when every Character Movement on the owner walks on a walkable floor that is not leaving. */
	bool IsCharacterStandingOnLoadedFloor() const;
	/** True when CurrentFloor no longer describes the floor under the capsule where it is now. */
	bool IsCachedFloorStale(const UCharacterMovementComponent& Movement) const;

	TArray<FEMSSuspendedBody> SuspendedBodies;
	TArray<FEMSSuspendedMovement> SuspendedMovements;
	/** Collision can remain registered after a level announces its departure. */
	TSet<TWeakObjectPtr<ULevel>> DepartingLevels;
	FDelegateHandle BeginInvisibleHandle;
	FDelegateHandle StateChangedHandle;
	FTimerHandle InitialEvaluationTimer;
	FTimerHandle SuspendedEvaluationTimer;
	FTimerHandle RelocationEvaluationTimer;
	bool bSuspended = false;
	uint64 StreamingHoldGeneration = 0;
	EEMSHoldReason HoldReason = EEMSHoldReason::Departure;
	/** When missing content last stopped being requested, or -1 while it is requested. */
	double UnrequestedSince = -1.0;
	/** Set by the last world-loading answer when nothing missing is being requested. */
	mutable bool bWaitingForUnrequestedContent = false;
	/** Set by the last world-loading answer when World Partition has nothing left to stream in below the owner. */
	mutable bool bPartitionLoadedBelow = false;

	/** The root whose transform updates reveal teleports. */
	TWeakObjectPtr<USceneComponent> ObservedRoot;
	FDelegateHandle RootTransformHandle;
	FVector LastObservedLocation = FVector::ZeroVector;

	/** One vertical probe line: where it starts and the Z its reach is measured from. */
	struct FEMSSupportProbeColumn
	{
		FVector Start = FVector::ZeroVector;
		double ReachOriginZ = 0.0;
		/** Columns from the same simulated primitive or capsule share this index. */
		int32 BodyIndex = INDEX_NONE;
		/** The body the column stands for, or null when only the actor's bounds were known. */
		const UPrimitiveComponent* Body = nullptr;
		/** Under a character's capsule, which its own floor check answers for. */
		bool bCharacterCapsule = false;
	};

	FTimerHandle DeferredMovementHoldTimer;

	void SuspendForReason(EEMSHoldReason Reason, bool bHoldMovement = true);
	void BeginHold(EEMSHoldReason Reason);
	/** Switches every holdable Character Movement to None and records its mode. */
	void HoldCharacterMovements();
	/** Finds missing content under OwnerBounds and whether any of it is requested for activation. */
	void InspectWorldPartitionContentBelow(
		const UWorldPartition& WorldPartition,
		const FBox& OwnerBounds,
		bool& bOutPending,
		bool& bOutRequested) const;
	void ObserveRoot();
	void StopObservingRoot();
	/** A relocation starts a new arrival grace period without borrowing physics flags again. */
	void ResetHoldForRelocation();
	void HandleRootTransformUpdated(
		USceneComponent* UpdatedComponent,
		EUpdateTransformFlags UpdateTransformFlags,
		ETeleportType Teleport);
	/** Drops movement holds that gameplay ended itself by changing the movement mode. */
	void ForgetExternallyReleasedMovement();

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
