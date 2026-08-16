//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAddonRestoreParticipant.h"
#include "EMSAddonsTypes.h"
#include "EMSActorSaveInterface.h"
#include "EMSGeometryCollectionTypes.h"
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

private:
	/** Corruption guard on saved transform counts, not a designer setting. */
	static constexpr int32 MaxPersistedTransforms = 100000;

	FTimerHandle RestoreRetryTimer;
	FEMSAddonResult LastRestoreResult;

	EEMSAddonRestorePhase RestorePhase = EEMSAddonRestorePhase::Idle;
	int32 RestoreRetryCount = 0;
	bool bGameThreadStateApplied = false;
	bool bIsEndingPlay = false;

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
