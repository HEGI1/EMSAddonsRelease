//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSActorSaveInterface.h"
#include "EMSAddonsAuthority.h"
#include "EMSAddonsTypes.h"
#include "EMSActorSpawnerTypes.h"
#include "GameFramework/Actor.h"
#include "UObject/ObjectKey.h"
#include "EMSActorSpawner.generated.h"

class UBillboardComponent;
class UEMSObject;
class USceneComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FEMSActorSpawnerRestoreStarted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMSActorSpawnerRestoreFinished, FEMSAddonResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FEMSSpawnedActorEvent,
	FEMSSpawnedActorId,
	ActorId,
	AActor*,
	Actor);

/**
 * Explicitly spawns runtime actors into its exact level and persists every
 * actor created through Spawn Actor using Easy Multi Save actor binary data.
 *
 * The spawner never creates actors automatically. Place one in the level and
 * call Spawn Actor on that specific instance whenever gameplay needs a new
 * persistent runtime actor.
 */
UCLASS(Blueprintable, ClassGroup = (EasyMultiSave), meta = (DisplayName = "EMS Actor Spawner"))
class EMSADDONSACTORSPAWNER_API AEMSActorSpawner : public AActor, public IEMSActorSaveInterface
{
	GENERATED_BODY()

public:
	AEMSActorSpawner();

	/**
	 * Effective authority for this non-replicated persistence actor.
	 *
	 * Client calls must never change the local manifest or spawn a divergent
	 * actor that the server will not save, which is the shared addon rule in
	 * EMSAddons::HasPersistenceAuthority rather than a spawner-specific one.
	 */
	bool HasAuthority() const
	{
		return EMSAddons::HasPersistenceAuthority(this, GetSpawnerNetMode());
	}

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Actor Spawner")
	TObjectPtr<USceneComponent> SpawnerRoot;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> SpawnerSprite;
#endif

	UPROPERTY(SaveGame, VisibleInstanceOnly, BlueprintReadOnly, Category = "EMS Addons|Actor Spawner")
	FGuid SpawnerId;

	UPROPERTY(SaveGame, VisibleInstanceOnly, Category = "EMS Addons|Actor Spawner")
	TArray<FEMSSpawnedActorRecord> SpawnedActorManifest;

	/**
	 * Read-only to Blueprint on purpose.
	 *
	 * Lowering this at runtime makes capture silently stop persisting live
	 * actors that have no record yet, and makes restoration skip every record
	 * past the new limit.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "EMS Addons|Actor Spawner|Limits", meta = (ClampMin = "1"))
	int32 MaxSpawnedActors = 1024;

	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Actor Spawner|Events")
	FEMSActorSpawnerRestoreStarted OnRestoreStarted;

	/**
	 * Signals that the spawner's synchronous reconciliation and binary-load pass
	 * has ended. Inspect the result for failed or skipped records.
	 *
	 * An actor that runs its own deferred restoration, such as an EMS Level
	 * Sequence or Geometry Collection actor, may still be settling. Ask that
	 * actor through EMS Addon Restore Participant when its completion matters.
	 */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Actor Spawner|Events")
	FEMSActorSpawnerRestoreFinished OnRestoreFinished;

	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Actor Spawner|Events")
	FEMSSpawnedActorEvent OnActorSpawned;

	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Actor Spawner|Events")
	FEMSSpawnedActorEvent OnActorRestored;

	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Actor Spawner|Events")
	FEMSSpawnedActorEvent OnActorRemoved;

	UFUNCTION(
		BlueprintCallable,
		Category = "EMS Addons|Actor Spawner",
		meta = (DisplayName = "Spawn Actor", DeterminesOutputType = "ActorClass"))
	AActor* SpawnActor(
		TSubclassOf<AActor> ActorClass,
		const FTransform& WorldTransform,
		FEMSSpawnedActorId& OutActorId);

	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Actor Spawner")
	bool DestroySpawnedActor(FEMSSpawnedActorId ActorId);

	UFUNCTION(BlueprintPure, Category = "EMS Addons|Actor Spawner")
	AActor* GetSpawnedActor(FEMSSpawnedActorId ActorId) const;

	UFUNCTION(BlueprintPure, Category = "EMS Addons|Actor Spawner")
	TArray<AActor*> GetAllSpawnedActors() const;

	UFUNCTION(BlueprintPure, Category = "EMS Addons|Actor Spawner")
	bool WasSpawnedByThis(const AActor* Actor) const;

	/**
	 * True while this spawner still owns a live-or-restorable record for the id.
	 *
	 * Distinguishes an actor that is merely not spawned right now from one that
	 * was destroyed for good, which GetSpawnedActor cannot express.
	 */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Actor Spawner")
	bool HasSpawnedActorRecord(FEMSSpawnedActorId ActorId) const;

	UFUNCTION(BlueprintPure, Category = "EMS Addons|Actor Spawner")
	bool GetSpawnedActorId(const AActor* Actor, FEMSSpawnedActorId& OutActorId) const;

	UFUNCTION(BlueprintPure, Category = "EMS Addons|Actor Spawner")
	bool HasFinishedRestoring() const;

	UFUNCTION(BlueprintPure, Category = "EMS Addons|Actor Spawner")
	FEMSAddonResult GetLastRestoreResult() const;

	/** Diagnostics. The durable identity this spawner tags its actors with. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Actor Spawner|Diagnostics")
	FGuid GetSpawnerIdentity() const;

	virtual void ActorPreSave_Implementation() override;
	virtual void ActorPreLoad_Implementation() override;
	virtual void ActorLoaded_Implementation() override;

#if WITH_EDITOR
	virtual void PostDuplicate(bool bDuplicateForPIE) override;
#endif

protected:
	/** Isolated so automation can exercise every net mode from one editor world. */
	virtual ENetMode GetSpawnerNetMode() const
	{
		return GetNetMode();
	}

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual UEMSObject* ResolveEMSObject() const;

	/**
	 * Serializes an actor into a standalone buffer.
	 *
	 * Deliberately unable to touch a record, so a failed capture cannot destroy
	 * the last good state. Isolated so automation can force a failure.
	 */
	virtual bool SaveActorState(AActor* Actor, TArray<uint8>& OutBinary);
	bool LoadActorState(const FEMSSpawnedActorRecord& Record, AActor* Actor);

private:
	UPROPERTY(Transient)
	TMap<FGuid, TObjectPtr<AActor>> LiveActorsById;

	TMap<FObjectKey, FGuid> LiveIdsByActor;
	/** Only ids whose saved state actually loaded, so On Actor Restored is honest. */
	TArray<FGuid> RestoredActorIds;
	FEMSAddonResult LastRestoreResult;

	FDelegateHandle ActorDestroyedHandle;
	EEMSSpawnerDestructionContext ActiveDestructionContext = EEMSSpawnerDestructionContext::None;
	bool bRestoreFinished = true;
	bool bRestoreHadFailure = false;
	bool bRestoreHadSkippedRecord = false;
	bool bIsEndingPlay = false;
	FGuid SpawnerIdentityBeforeLoad;

	bool CanMutateSpawnedActors() const;
	bool IsRecordValid(const FEMSSpawnedActorRecord& Record, FText& OutReason) const;
	void EnsureSpawnerIdentity();
	void RetagLiveActorsForIdentityChange(const FGuid& PreviousIdentity);
	void EnsureActorDestroyedHandler();
	void RemoveActorDestroyedHandler();

	AActor* SpawnActorWithId(
		TSubclassOf<AActor> ActorClass,
		FEMSSpawnedActorId ActorId,
		const FTransform& WorldTransform);
	/** By value on purpose: spawning can move the manifest. See the definition. */
	AActor* SpawnActorForRecord(FEMSSpawnedActorRecord Record);
	void ApplyRecordState(AActor* Actor, const FEMSSpawnedActorRecord& Record) const;
	void ApplyRecordAttachment(AActor* Actor, const FEMSSpawnedActorRecord& Record) const;

	bool RegisterLiveActor(const FGuid& LocalId, AActor* Actor);
	void UnregisterLiveActor(AActor* Actor);
	void RebuildLiveBindings();
	void PruneObsoleteAbsentRecords();
	void CaptureLiveActorsForSave();
	void UpdateRecordFromActor(FEMSSpawnedActorRecord& Record, AActor* Actor);
	void CaptureSpawnerStateOnGameThread();

	void RestoreSpawnedActors();
	void FinishRestore();

	bool DestroyLiveActor(const FGuid& LocalId, EEMSSpawnerDestructionContext Context, bool bMarkAbsent);

	FEMSSpawnedActorRecord* FindRecordMutable(const FGuid& LocalId);
	const FEMSSpawnedActorRecord* FindRecord(const FGuid& LocalId) const;

	static FName MakeStableActorName(const FGuid& LocalId);
	bool TryGetOwnerIdFromActorTag(const AActor* Actor, FGuid& OutId) const;
	bool TryGetChildIdFromActorTag(const AActor* Actor, FGuid& OutId) const;

	UFUNCTION()
	void HandleChildEndPlay(AActor* Actor, EEndPlayReason::Type EndPlayReason);

	void HandleChildDestroyed(AActor* Actor);
};
