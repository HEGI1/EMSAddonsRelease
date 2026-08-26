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
 * The guard reacts to the two streaming edges instead of polling. It suspends
 * only when the level that is actually holding the owner up is about to become
 * invisible, or when nothing at all is loaded below the owner at Begin Play in a
 * world that actually streams (World Partition streaming or a conventional
 * streamed sublevel) - a static world has nothing to explain missing support
 * with, so it never runs that startup check. It resumes as soon as a level
 * becomes visible again and something is back under the owner. Ordinary
 * gameplay - debris falling into a pit, an object knocked off a ledge - never
 * reaches either condition.
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

	/** Isolated so automation can exercise the guard without a streaming world. */
	virtual bool HasAnySupportBelow() const;
	virtual bool IsSupportedByLevel(const ULevel* Level) const;

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
	FDelegateHandle BeginInvisibleHandle;
	FDelegateHandle StateChangedHandle;
	FTimerHandle InitialEvaluationTimer;
	bool bSuspended = false;

	bool TraceBelowOwner(double Reach, FHitResult& OutHit) const;

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
