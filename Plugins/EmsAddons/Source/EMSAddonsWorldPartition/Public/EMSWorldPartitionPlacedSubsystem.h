//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSWorldPartitionPlacedTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"
#include "EMSWorldPartitionPlacedSubsystem.generated.h"

class AEMSWorldPartitionRuntimeManager;
class UEMSObject;
class UEMSWorldPartitionPlacedComponent;
class ULevel;
class ULevelStreaming;
enum class ELevelStreamingState : uint8;

/** Owns placed actors only; runtime-spawned actors keep their existing subsystem. */
UCLASS()
class EMSADDONSWORLDPARTITION_API UEMSWorldPartitionPlacedSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	bool RegisterPlacedActor(UEMSWorldPartitionPlacedComponent* Component);
	void HandleManagerPreSave(AEMSWorldPartitionRuntimeManager* Manager);
	void HandleManagerPreLoad(AEMSWorldPartitionRuntimeManager* Manager);
	void HandleManagerLoaded(AEMSWorldPartitionRuntimeManager* Manager);
	bool IsRestoringActor() const { return bRestoringActor; }
	bool IsRemovingManagedActor() const { return bInternalRemoval; }
	bool IsRemovingManagedActor(const AActor* Actor) const { return Actor && InternalRemovalActor == Actor; }

	UFUNCTION(BlueprintPure, Category = "EMS Addons|World Partition Placed Actors")
	bool IsPlacedActorManaged(const AActor* Actor) const;

	UFUNCTION(BlueprintCallable, Category = "EMS Addons|World Partition Placed Actors")
	TArray<AActor*> GetManagedPlacedActors() const;

	void Reconcile();

#if WITH_DEV_AUTOMATION_TESTS
	bool CaptureAndRemoveForTesting(const FGuid& Id) { return CanOperate() && !IsEMSBusy() && Capture(Id) && RemoveLiveActor(Id); }
#endif

private:
	static constexpr int32 MaxPlacedActorRecords = 4096;
	TWeakObjectPtr<AEMSWorldPartitionRuntimeManager> PrimaryManager;
	TWeakObjectPtr<UEMSObject> BoundEMSObject;
	TMap<FGuid, TWeakObjectPtr<AActor>> LiveActors;
	TMap<FObjectKey, FGuid> LiveIds;
	/** Authored baseline retained for loading older saves and resetting the same map. */
	TMap<FGuid, FEMSWorldPartitionPlacedActorRecord> AuthoredDefaults;
	TMap<FGuid, TWeakObjectPtr<AActor>> AuthoredActors;
	TMap<FObjectKey, FGuid> AuthoredIds;
	TMap<FName, FGuid> AuthoredIdsByName;
	TSet<FGuid> RejectedIds;
	// These are independent of the manager array: EMS may serialize that array on
	// a worker while a streaming edge or gameplay destruction occurs on the GT.
	TMap<FGuid, FEMSWorldPartitionPlacedActorRecord> DeferredDormancy;
	TMap<FGuid, FGuid> DeferredSupportCells;
	TSet<FGuid> DeferredDestroyedIds;
	uint64 LoadGeneration = 0;
	TArray<TWeakObjectPtr<UEMSWorldPartitionPlacedComponent>> PendingPlacements;
	FDelegateHandle BeginInvisibleHandle;
	FDelegateHandle StateChangedHandle;
	FDelegateHandle DestroyedHandle;
	FDelegateHandle SpawnedHandle;
	FTimerHandle ReconcileTimer;
	bool bDeinitializing = false;
	bool bInternalRemoval = false;
	AActor* InternalRemovalActor = nullptr;
	bool bRestoringActor = false;
	bool bLoading = false;
	bool bPendingLoadReset = false;
	bool bManagerLoadedSinceCompletion = false;
	bool bHasLoadedPlacedState = false;
	bool bReconciling = false;
	bool bCapturing = false;

	bool CanOperate() const;
	AEMSWorldPartitionRuntimeManager* GetManager();
	bool IsEMSBusy() const;
	FEMSWorldPartitionPlacedActorRecord* FindRecord(const FGuid& Id);
	bool Capture(const FGuid& Id);
	void ApplyDeferredChanges();
	bool RemoveLiveActor(const FGuid& Id);
	bool Restore(FEMSWorldPartitionPlacedActorRecord Record);
	void ApplyAuthoredDefaults();
	void HandleActorDestroyed(AActor* Actor);
	void HandleActorSpawned(AActor* Actor);

	UFUNCTION()
	void HandlePersistentLevelLoaded(const TArray<TSoftObjectPtr<AActor>>& LoadedActors);

	void HandleLevelBeginMakingInvisible(UWorld* World, const ULevelStreaming* Streaming, ULevel* Level);
	void HandleLevelStreamingStateChanged(UWorld* World, const ULevelStreaming* Streaming, ULevel* Level,
		ELevelStreamingState PreviousState, ELevelStreamingState NewState);
};
