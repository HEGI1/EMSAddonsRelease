//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAddonRestoreParticipant.h"
#include "EMSAddonsTypes.h"
#include "EMSActorSaveInterface.h"
#include "EMSGeometryCollectionTypes.h"
#include "EMSPhysicsStreamingGuardComponent.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GameFramework/Actor.h"
#include "EMSGeometryCollectionActor.generated.h"

UCLASS(
	Blueprintable,
	ClassGroup = (EasyMultiSave),
	meta = (DisplayName = "EMS Geometry Collection Actor"))
class EMSADDONSGEOMETRY_API AEMSGeometryCollectionActor
	: public AActor
	, public IEMSActorSaveInterface
	, public IEMSAddonRestoreParticipant
{
	GENERATED_BODY()

public:
	AEMSGeometryCollectionActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Geometry")
	TObjectPtr<UGeometryCollectionComponent> GeometryCollectionComponent;

	/**
	 * Keeps the collection from falling out of the world while the geometry it
	 * rests on is streamed out. Placed here rather than left to the project
	 * because a saved position below the world is a persistence problem, not only
	 * a physics one.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Geometry")
	TObjectPtr<UEMSPhysicsStreamingGuardComponent> StreamingPhysicsGuard;

#if WITH_EDITORONLY_DATA
	/** Editor-only marker, so the actor is identifiable before an asset is assigned. */
	UPROPERTY()
	TObjectPtr<class UBillboardComponent> GeometrySprite;
#endif

	UPROPERTY(SaveGame, VisibleInstanceOnly, Category = "EMS Addons|Geometry|Advanced", meta = (AdvancedDisplay))
	FEMSGeometryCollectionState PersistedState;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EMS Addons|Geometry|Compatibility")
	EEMSGeometryAssetMismatchPolicy AssetMismatchPolicy =
		EEMSGeometryAssetMismatchPolicy::Reject;

	/**
	 * Retry budget while waiting for geometry and physics runtime resources.
	 *
	 * Internal tuning rather than a designer setting, in the same spirit as
	 * MaxPersistedTransforms below. Left assignable so automation can force a
	 * fast timeout.
	 */
	int32 MaxRestoreRetries = 5;

	/**
	 * Minimum world time, in seconds, to wait for runtime resources before a
	 * restore times out. Both this and MaxRestoreRetries must be exhausted, so a
	 * streaming hitch that delays the physics proxy by a few frames cannot end the
	 * restore. Same internal-tuning status as MaxRestoreRetries.
	 */
	float RestoreTimeoutSeconds = 5.0f;

	/**
	 * Restore status for C++ callers.
	 *
	 * Blueprint reads the same values through EMS Addon Restore Participant,
	 * so these are deliberately not exposed a second time.
	 */
	bool IsGeometryRestorePending() const;
	FEMSAddonResult GetGeometryRestoreResult() const;

	virtual void ActorPreSave_Implementation() override;
	virtual void ActorPreLoad_Implementation() override;
	virtual void ActorLoaded_Implementation() override;

	virtual bool IsEMSAddonRestoreComplete() const override;
	virtual FEMSAddonResult GetEMSAddonRestoreResult() const override;

protected:
	bool CaptureGeometryCollectionState();
	bool RestoreGeometryCollectionState();
	bool ValidateGeometryCollectionState(FText& OutReason) const;
	EEMSAddonRestorePhase GetGeometryRestorePhase() const;
	int64 ComputeCurrentHierarchyHash() const;

	/** Isolated so automation can exercise every net mode from one editor world. */
	virtual ENetMode GetGeometryNetMode() const
	{
		return GetNetMode();
	}

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;
	virtual bool IsGeometryRuntimeReadyForRestore() const;
	virtual bool RequiresPhysicsProxySynchronization() const;
	virtual bool IsPhysicsRuntimeReadyForRestore() const;
	virtual bool SynchronizePhysicsThreadState();
	virtual FGeometryDynamicCollection* GetPersistenceDynamicCollection();
	virtual const FGeometryDynamicCollection* GetPersistenceDynamicCollection() const;
	void AttemptRestore();

	/**
	 * World time, so the restore budget pauses with the retry timers. Virtual so
	 * automation can expire the budget in an editor world, whose time never moves.
	 */
	virtual double GetRestoreClockSeconds() const;

private:
	/** Corruption guard on saved transform counts, not a designer setting. */
	static constexpr int32 MaxPersistedTransforms = 100000;

	FTimerHandle RestoreRetryTimer;
	FEMSAddonResult LastRestoreResult;

	EEMSAddonRestorePhase RestorePhase = EEMSAddonRestorePhase::Idle;
	double RestoreStartSeconds = 0.0;
	int32 RestoreRetryCount = 0;
	bool bGameThreadStateApplied = false;
	bool bIsEndingPlay = false;

	/**
	 * Set when a restore timed out before it touched the collection.
	 *
	 * The live collection is then still the authored one, and capturing it would
	 * overwrite the saved state that never got the chance to apply. Capture keeps
	 * PersistedState until a later restore begins.
	 */
	bool bPreserveUnrestoredState = false;

	void FailRestoreKeepingSavedState(const FText& Reason);

	void SkipPersistenceWithoutAuthority();
	bool CanAccessGeometryState(FText& OutReason) const;
	bool ValidateStateInternal(FText& OutReason, bool bRequireDynamicCollection) const;
	bool ApplyGameThreadState(FText& OutReason);
	void MarkRestoreUnsafeAndFail(const FText& Reason);
	void ScheduleRestoreRetry();
	void FinalizeRestore();
	void FailRestore(const FText& Reason);
	void CancelPendingRestore(bool bReportCanceled);
	void RefreshGeometryRendering();
	void LogRestoreIssue(const FText& Reason) const;
};
