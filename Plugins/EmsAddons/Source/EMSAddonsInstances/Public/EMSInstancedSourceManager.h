//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSActorSaveInterface.h"
#include "EMSAddonRestoreParticipant.h"
#include "EMSAddonsTypes.h"
#include "EMSInstancedSourceTypes.h"
#include "EMSInstanceTransformUtils.h"
#include "GameFramework/Actor.h"
#include "Templates/Function.h"
#include "UObject/ObjectKey.h"
#include "EMSInstancedSourceManager.generated.h"

class UBillboardComponent;
class UInstancedStaticMeshComponent;
class ULevel;

/**
 * Shared persistence for instanced static mesh sources.
 *
 * The base owns source bookkeeping, the sparse authored-baseline delta format,
 * capture, validation, current-to-target reconciliation, and the component
 * mutations that reconciliation applies. A subclass supplies only the one thing
 * that actually differs per source type: how sources are discovered. Everything
 * that differs per source after that, such as the space its transforms are
 * keyed in, is derived from the source identity rather than from the manager,
 * so one manager can own several kinds of source at once.
 *
 * Persistent identity is source plus canonical transform plus duplicate
 * ordinal. Moving an instance changes identity. Exact overlapping duplicates
 * retain their layout and state through manager operations, but cannot have
 * durable semantic identity after arbitrary external reordering.
 *
 * Gameplay adds and removes instances on the components directly. A permanent
 * change of state is expressed as two ordinary instance edits, such as removing
 * a tree from the intact source and adding a stump to a second source, and both
 * are captured by the same delta. This manager therefore never creates, owns, or
 * hides actors, and needs no Actor Spawner.
 *
 * Place exactly one manager in the persistent level.
 */
UCLASS(Abstract)
class EMSADDONSINSTANCES_API AEMSInstancedSourceManager
	: public AActor
	, public IEMSActorSaveInterface
	, public IEMSAddonRestoreParticipant
{
	GENERATED_BODY()

public:
	AEMSInstancedSourceManager();

	/**
	 * Restore status for C++ callers.
	 *
	 * Blueprint reads the same values through EMS Addon Restore Participant,
	 * so these are deliberately not exposed a second time.
	 */
	bool IsInstancedRestorePending() const;
	FEMSAddonResult GetInstancedRestoreResult() const;

	/**
	 * Outcome of the most recent capture.
	 *
	 * Skipped reports that at least one loaded source could not be captured and
	 * kept its previous delta. The save itself still succeeds, so this is the
	 * only way to tell a complete capture from a partial one.
	 */
	FEMSAddonResult GetInstancedCaptureResult() const;
	int32 GetSkippedCaptureSourceCount() const;

	/** Finds the active manager of the given kind in the world, if any. */
	UFUNCTION(
		BlueprintPure,
		Category = "EMS Addons|Instanced",
		meta = (
			WorldContext = "WorldContextObject",
			DeterminesOutputType = "ManagerClass",
			DisplayName = "Get EMS Instanced Source Manager"))
	static AEMSInstancedSourceManager* GetInstancedSourceManager(
		const UObject* WorldContextObject,
		TSubclassOf<AEMSInstancedSourceManager> ManagerClass);

	/**
	 * Per-instance gameplay data.
	 *
	 * Instances are addressed the way a line trace reports them, by component
	 * plus current instance index. The index is only used to resolve the current
	 * transform key, so it never has to survive a save and a move changes the
	 * instance's persistent identity.
	 */

	UFUNCTION(BlueprintPure, Category = "EMS Addons|Instanced|Gameplay Data")
	bool GetInstanceGameplayValue(
		const UInstancedStaticMeshComponent* Component,
		int32 InstanceIndex,
		FGameplayTag Tag,
		float& OutValue) const;

	UFUNCTION(BlueprintPure, Category = "EMS Addons|Instanced|Gameplay Data")
	bool GetInstanceGameplayData(
		const UInstancedStaticMeshComponent* Component,
		int32 InstanceIndex,
		FEMSInstanceGameplayData& OutGameplayData) const;

	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Instanced|Gameplay Data")
	bool SetInstanceGameplayValue(
		UInstancedStaticMeshComponent* Component,
		int32 InstanceIndex,
		FGameplayTag Tag,
		float Value);

	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Instanced|Gameplay Data")
	bool SetInstanceGameplayData(
		UInstancedStaticMeshComponent* Component,
		int32 InstanceIndex,
		const FEMSInstanceGameplayData& GameplayData);

	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Instanced|Gameplay Data")
	bool RemoveInstanceGameplayValue(
		UInstancedStaticMeshComponent* Component,
		int32 InstanceIndex,
		FGameplayTag Tag);

	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Instanced|Gameplay Data")
	bool ClearInstanceGameplayData(
		UInstancedStaticMeshComponent* Component,
		int32 InstanceIndex);

	virtual void ActorPreSave_Implementation() override;
	virtual void ActorPreLoad_Implementation() override;
	virtual void ActorLoaded_Implementation() override;

#if WITH_EDITOR
	/** The manager only works from the persistent level, so the flag is fixed. */
	virtual bool CanChangeIsSpatiallyLoadedFlag() const override
	{
		return false;
	}
#endif

	virtual bool IsEMSAddonRestoreComplete() const override;
	virtual FEMSAddonResult GetEMSAddonRestoreResult() const override;

protected:
	UPROPERTY(
		SaveGame,
		VisibleInstanceOnly,
		Category = "EMS Addons|Instanced|Advanced",
		meta = (AdvancedDisplay))
	TArray<FEMSInstanceSourceDelta> SavedDeltas;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> ManagerSprite;
#endif

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	void RefreshLoadedSources();

	/**
	 * Treats every collected source as already matching what a restore produces.
	 *
	 * Sources present at BeginPlay are in their authored state, which is exactly
	 * what reconciling against an empty save would produce. Without this the
	 * first level to stream in reconciles live gameplay changes away before
	 * anything has been saved or loaded, because nothing else marks a source as
	 * settled until a load runs.
	 */
	void MarkLoadedSourcesRestored();

	/**
	 * Captures the current delta for every loaded source.
	 *
	 * Returns false when the capture was refused, which leaves the previous
	 * SavedDeltas in place rather than replacing good data with a partial pass.
	 *
	 * OnlyLevel restricts the pass to the sources that level owns and skips the
	 * discovery pass, which is what an unloading level needs. Rebuilding the
	 * resolved state of every source in the world on each streaming event is the
	 * one thing here that costs a frame in a foliage-heavy World Partition map.
	 * A level-scoped pass is bookkeeping rather than a save, so it also leaves
	 * the reported capture result alone.
	 */
	bool CaptureDeltas(const ULevel* OnlyLevel = nullptr);
	void RestoreLoadedSources();
	bool ActivateAsWorldManager();
	void RemoveSourcesInLevel(ULevel* Level);

	virtual void CollectSources()
		PURE_VIRTUAL(AEMSInstancedSourceManager::CollectSources, );

	/**
	 * Whether a source's instances are read and written in world space.
	 *
	 * Painted foliage is authored and keyed in world space; ordinary ISM and
	 * HISM instances are keyed relative to their owner. This is derived from the
	 * kind already stored in the identity rather than being a property of the
	 * manager, so one manager can hold both kinds of source at once.
	 */
	static bool UsesWorldSpaceTransforms(const FEMSInstanceSourceId& SourceId)
	{
		return SourceId.SourceKind == EEMSInstanceSourceKind::Foliage;
	}

	/** Isolated so automation can exercise every net mode from one world. */
	virtual ENetMode GetInstanceNetMode() const;

	void RegisterSource(
		const FEMSInstanceSourceId& SourceId,
		UInstancedStaticMeshComponent* Component);

	/**
	 * Whether an owner's object path still identifies the same object next session.
	 *
	 * Source identity is level plus owner path plus component name, so it only
	 * holds for actors that came from the map. A runtime-spawned owner is named
	 * from a per-session counter: its delta never matches the same actor again,
	 * and the next session can hand that generated name to an unrelated actor
	 * which would then inherit the delta. Such a source is left out entirely
	 * rather than persisted under an identity that does not survive.
	 *
	 * Uses the same placed-actor rule as EMS itself, so the addon persists the
	 * sources EMS already treats as level-placed. Counts rejections for the
	 * summary RefreshLoadedSources logs, which is why it is not const.
	 */
	bool IsStableSourceOwner(const AActor* SourceOwner);

	static FName MakeLevelIdentity(ULevel* Level);

	void LogSourceIssue(
		const FEMSInstanceSourceId& SourceId,
		const FText& Reason) const;

private:
	struct FResolvedInstance
	{
		FTransform Transform = FTransform::Identity;
		EMSAddons::Instances::FCanonicalTransform Canonical;
		FEMSInstanceKey Key;
		TArray<float> CustomData;
		FEMSInstanceGameplayData GameplayData;
		int32 RuntimeIndex = INDEX_NONE;
	};

	struct FResolvedSourceState
	{
		TArray<FResolvedInstance> Instances;
		uint64 BaselineSignature = 0;
		int32 CustomDataFloatCount = 0;
		bool bOrdinalOverflow = false;
	};

	/**
	 * Runtime index to persistent key, built once per component instead of per
	 * lookup.
	 *
	 * The duplicate ordinal is positional, so resolving one instance's key used
	 * to rescan every earlier instance. That made a single Blueprint call on a
	 * large foliage component walk the whole instance list.
	 *
	 * Rebuilt whenever the instance count changes or the queried instance no
	 * longer canonicalizes to what was cached, and dropped outright on any
	 * mutation the manager performs.
	 *
	 * The one case those checks do not catch is gameplay moving an instance that
	 * exactly overlapped the queried one, which shifts the queried instance's
	 * ordinal without changing its own transform. That is already outside the
	 * documented guarantee: exact duplicates hold no durable individual identity
	 * across external reordering.
	 */
	struct FInstanceKeyCache
	{
		int32 InstanceCount = 0;
		TArray<EMSAddons::Instances::FCanonicalTransform> CanonicalByRuntimeIndex;
		TArray<FEMSInstanceKey> KeysByRuntimeIndex;
	};

	static constexpr int32 MaxSourceDeltas = 4096;
	static constexpr int32 MaxTotalChangedInstances = 500000;
	static constexpr int32 MaxCustomDataFloatsPerInstance = 64;
	static constexpr int32 MaxGameplayValuesPerInstance = 32;

	TMap<FEMSInstanceSourceId, TWeakObjectPtr<UInstancedStaticMeshComponent>>
		LoadedSources;
	TMap<FEMSInstanceSourceId, FResolvedSourceState> SourceBaselines;
	TMap<FEMSInstanceSourceId, TWeakObjectPtr<UInstancedStaticMeshComponent>>
		RestoredSources;
	TSet<FEMSInstanceSourceId> AmbiguousSources;
	TMap<FObjectKey, FEMSInstanceSourceId> SourceIdsByComponent;
	TMap<FEMSInstanceSourceId, TMap<FEMSInstanceKey, FEMSInstanceGameplayData>>
		LiveGameplayData;
	mutable TMap<FObjectKey, FInstanceKeyCache> InstanceKeyCaches;
	FDelegateHandle LevelAddedHandle;
	FDelegateHandle LevelRemovedHandle;
	FEMSAddonResult LastRestoreResult;
	FEMSAddonResult LastCaptureResult;
	int32 SkippedCaptureSources = 0;
	int32 UnstableSourceOwners = 0;

	/** Frame of the last discovery pass, so a recovery pass runs at most once. */
	uint64 LastSourceRefreshFrame = 0;

	bool bIsActiveManager = false;
	bool bRestorePending = false;
	bool bIsEndingPlay = false;

	/** Whether any source was skipped since the current load began. */
	bool bHadSkippedRestoreSource = false;

	FResolvedSourceState BuildResolvedSourceState(
		const UInstancedStaticMeshComponent* Component,
		bool bWorldSpace) const;
	void AttachAndPruneGameplayData(
		const FEMSInstanceSourceId& SourceId,
		FResolvedSourceState& InOutState);
	FEMSInstanceSourceDelta BuildDelta(
		const FEMSInstanceSourceId& SourceId,
		const FResolvedSourceState& Baseline,
		const FResolvedSourceState& Current) const;
	FResolvedSourceState BuildTargetState(
		const FResolvedSourceState& Baseline,
		const FEMSInstanceSourceDelta* Delta) const;
	bool ValidateDelta(
		const FEMSInstanceSourceDelta& Delta,
		const FResolvedSourceState& Baseline,
		int32& InOutTotalChanges) const;
	bool ReconcileSource(
		const FEMSInstanceSourceId& SourceId,
		const FEMSInstanceSourceDelta* Delta,
		UInstancedStaticMeshComponent* Component,
		const FResolvedSourceState& Baseline);
	void RebuildGameplayData(
		const FEMSInstanceSourceId& SourceId,
		const FResolvedSourceState& Target,
		const FResolvedSourceState& Verified);
	void HandleLevelAdded(ULevel* Level, UWorld* World);
	void HandleLevelRemoved(ULevel* Level, UWorld* World);

	void ForgetSource(const FEMSInstanceSourceId& SourceId);

	/**
	 * Applies one reconciliation pass to a component.
	 *
	 * Painted foliage needs the same three operations an ordinary HISM does, so
	 * this is shared rather than overridden. Removal indices arrive sorted in
	 * reverse, which is what the second RemoveInstances parameter asserts.
	 */
	bool ApplyInstanceMutations(
		UInstancedStaticMeshComponent* Component,
		const TArray<int32>& InstanceIndicesToRemove,
		const TArray<FTransform>& TransformsToAdd,
		bool bWorldSpace) const;

	/**
	 * Whether this machine may change authoritative instance state.
	 *
	 * Applies the shared EMSAddons::HasPersistenceAuthority rule to the net mode
	 * this manager reports.
	 */
	bool HasInstanceAuthority() const;

	const FInstanceKeyCache* GetInstanceKeyCache(
		const UInstancedStaticMeshComponent* Component,
		int32 QueriedInstanceIndex,
		const EMSAddons::Instances::FCanonicalTransform& QueriedCanonical,
		bool bWorldSpace) const;
	void InvalidateInstanceKeyCaches();

	bool ResolveCurrentInstanceKey(
		const UInstancedStaticMeshComponent* Component,
		int32 InstanceIndex,
		FEMSInstanceSourceId& OutSourceId,
		FEMSInstanceKey& OutKey) const;
	bool IsValidGameplayData(const FEMSInstanceGameplayData& GameplayData) const;
	bool WriteInstanceGameplayData(
		UInstancedStaticMeshComponent* Component,
		int32 InstanceIndex,
		TFunctionRef<bool(FEMSInstanceGameplayData&)> Mutator);
};
