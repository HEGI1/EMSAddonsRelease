//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"
#include "EMSWorldPartitionRuntimeSubsystem.generated.h"

class AEMSWorldPartitionRuntimeManager;
class UEMSObject;
class ULevel;
class ULevelStreaming;
class UWorldPartitionRuntimeLevelStreamingCell;
enum class ELevelStreamingState : uint8;

UCLASS()
class EMSADDONSWORLDPARTITION_API UEMSWorldPartitionRuntimeSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

	AActor* SpawnManagedActor(
		TSubclassOf<AActor> ActorClass,
		const FTransform& WorldTransform,
		ESpawnActorCollisionHandlingMethod CollisionHandlingOverride);

	bool IsManagedActor(const AActor* Actor) const;

	/**
	 * True while this subsystem is itself removing a managed actor.
	 *
	 * An actor reads this from End Play to tell "the addon is taking me out of the
	 * world" from "gameplay destroyed me", which look identical otherwise. Both
	 * streaming out and a load replacing the records report true.
	 */
	bool IsRemovingManagedActor() const { return bSuppressGameplayDestruction; }
	void GetManagedActors(TArray<AActor*>& OutActors) const;
	int32 GetManagedActorCount() const;
	int32 GetDormantActorCount() const;
	void Reconcile();

	void HandleManagerPreSave(AEMSWorldPartitionRuntimeManager* Manager);
	void HandleManagerPreLoad(AEMSWorldPartitionRuntimeManager* Manager);
	void HandleManagerLoaded(AEMSWorldPartitionRuntimeManager* Manager);
	bool IsPrimaryManager(const AEMSWorldPartitionRuntimeManager* Manager) const;

	static FName MakeStableActorName(const FGuid& LocalId);

#if WITH_DEV_AUTOMATION_TESTS
	void SetCaptureFailureForTesting(bool bFail) { bForceCaptureFailureForTests = bFail; }
	void SetDestroyFailureForTesting(bool bFail) { bForceDestroyFailureForTests = bFail; }
	void SetCellValidationBypassForTesting(bool bBypass) { bBypassCellValidationForTests = bBypass; }
	bool DestroyManagedActorForTesting(const AActor* Actor);

	/** Whether the manager's last capture entry point reached this on the game thread. */
	bool WasLastManagerPreSaveOnGameThread() const { return bLastManagerPreSaveOnGameThread; }
#endif

private:
	static constexpr int32 MaxRuntimeActorRecords = 4096;

	TWeakObjectPtr<AEMSWorldPartitionRuntimeManager> PrimaryManager;
	TMap<FGuid, TWeakObjectPtr<AActor>> LiveActorsById;
	TMap<FObjectKey, FGuid> LiveIdsByActor;
	FDelegateHandle ActorDestroyedHandle;
	FDelegateHandle BeginInvisibleHandle;
	FDelegateHandle StateChangedHandle;
	FTimerHandle ReconcileTimerHandle;
	TWeakObjectPtr<UEMSObject> BoundEMSObject;

	/**
	 * True while the subsystem is itself removing a managed actor.
	 *
	 * Streaming out, reloading, restoring, and shutdown all destroy actors, and
	 * what they have in common is the only thing the destroyed handler needs: the
	 * removal is not gameplay's doing and must not drop the durable record.
	 */
	bool bSuppressGameplayDestruction = false;
	bool bDeinitializing = false;
	bool bDelegatesBound = false;
	bool bPrimaryManagerLoadedSincePersistentLoadCompletion = false;
	double LastUnresolvedCellWarningTime = -TNumericLimits<double>::Max();
#if WITH_DEV_AUTOMATION_TESTS
	bool bForceCaptureFailureForTests = false;
	bool bForceDestroyFailureForTests = false;
	bool bBypassCellValidationForTests = false;
	bool bLastManagerPreSaveOnGameThread = false;
#endif

	bool CanOperate() const;
	bool EnsureReady();
	void RefreshPrimaryManager(bool bCreateIfMissing);
	void BindDelegates();
	void UnbindDelegates();
	void BindEMSDelegate();
	void UnbindEMSDelegate();

	UFUNCTION()
	void HandlePersistentLevelLoaded(const TArray<TSoftObjectPtr<AActor>>& LoadedActors);

	bool RegisterLiveActor(const FGuid& LocalId, AActor* Actor);
	void UnregisterLiveActor(AActor* Actor);
	void HandleActorDestroyed(AActor* Actor);

	bool SaveActorState(AActor* Actor, TArray<uint8>& OutBinary) const;
	bool LoadActorState(AActor* Actor, const TArray<uint8>& Binary, bool bDeferredSpawn = false) const;
	bool CaptureActor(const FGuid& LocalId, AActor* Actor);
	bool CaptureAndRemoveActor(const FGuid& LocalId);
	bool DestroyActorInternally(const FGuid& LocalId);
	void DiscardActor(AActor* Actor);
	bool RestoreRecord(struct FEMSWorldPartitionRuntimeActorRecord Record);
	void RestoreVisibleDormantRecords();

	struct FEMSWorldPartitionRuntimeActorRecord* FindRecordMutable(const FGuid& LocalId);
	const struct FEMSWorldPartitionRuntimeActorRecord* FindRecord(const FGuid& LocalId) const;
	bool ValidateRecord(const struct FEMSWorldPartitionRuntimeActorRecord& Record, UClass*& OutClass) const;

	const UWorldPartitionRuntimeLevelStreamingCell* GetCellFromStreamingLevel(const ULevelStreaming* StreamingLevel) const;
	void HandleLevelBeginMakingInvisible(UWorld* InWorld, const ULevelStreaming* StreamingLevel, ULevel* LoadedLevel);
	void HandleLevelStreamingStateChanged(
		UWorld* InWorld,
		const ULevelStreaming* StreamingLevel,
		ULevel* LoadedLevel,
		ELevelStreamingState PreviousState,
		ELevelStreamingState NewState);
};
