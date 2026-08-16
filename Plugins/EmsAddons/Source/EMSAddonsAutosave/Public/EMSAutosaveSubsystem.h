//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAutosaveRequest.h"
#include "EMSAutosaveTypes.h"
#include "EMSTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "EMSAutosaveSubsystem.generated.h"

class AActor;
class AEMSCheckpoint;
class APlayerController;
class UEMSAsyncLoadGame;
class UEMSAsyncSaveGame;
class UEMSCheckpointSaveGame;
class UEMSObject;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMSAutosaveEvent, FName, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMSCheckpointEvent, FEMSCheckpointRecord, Checkpoint);

/** Game-instance service that coordinates addon autosaves and per-slot checkpoints through normal EMS operations. */
UCLASS()
class EMSADDONSAUTOSAVE_API UEMSAutosaveSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Fired immediately before an addon-requested EMS save starts. Reason identifies the request source. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSAutosaveEvent OnAutosaveStarted;

	/** Fired after an addon-requested EMS save finishes successfully. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSAutosaveEvent OnAutosaveCompleted;

	/** Fired when an addon-requested autosave cannot be completed. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSAutosaveEvent OnAutosaveFailed;

	/** Fired when a checkpoint activation is accepted by the subsystem. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSCheckpointEvent OnCheckpointActivated;

	/** Fired after both the EMS save and checkpoint metadata have been committed successfully. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSCheckpointEvent OnCheckpointCommitted;

	/** Fired after the latest checkpoint has finished loading through EMS. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSCheckpointEvent OnCheckpointLoaded;

	/** Fired when loading the latest checkpoint fails or is canceled. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSCheckpointEvent OnCheckpointLoadFailed;

	/**
	 * Queues a normal EMS async save using the data groups configured for the checkpoint system.
	 * Returns false while EMS is loading; load/restore is a cancellation boundary and autosaves are not queued across it.
	 * This is a convenience scheduler and does not replace normal EMS saves.
	 */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Autosave", meta = (DisplayName = "Request Autosave"))
	bool RequestAutosave(FName Reason = NAME_None);

	/**
	 * Changes the periodic autosave interval for the current game session.
	 * The value is in seconds; zero disables periodic autosaves. Project config is not written to disk.
	 */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Autosave", meta = (DisplayName = "Set Periodic Autosave Interval", ClampMin = "0.0", Units = "s"))
	void SetPeriodicAutosaveInterval(float IntervalSeconds);

	/** Returns the periodic autosave interval currently used by the subsystem, in seconds. Zero means disabled. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Autosave and Checkpoints|Autosave", meta = (DisplayName = "Get Periodic Autosave Interval", Units = "s"))
	float GetPeriodicAutosaveInterval() const;

	/** C++ entry used by EMS Checkpoint actors. Blueprint activates the checkpoint actor itself. */
	bool ActivateCheckpoint(AEMSCheckpoint* Checkpoint);

	/**
	 * Loads the latest committed checkpoint for the current EMS save slot.
	 * Pending addon autosaves are discarded before restore so old requests cannot save again immediately after loading.
	 * The system travels to the checkpoint world when required and lets EMS restore the saved player/level state normally.
	 * After EMS finishes, the local player is placed at the matching checkpoint trigger center while keeping the EMS-restored rotation.
	 */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Checkpoint", meta = (DisplayName = "Load Last Checkpoint"))
	bool LoadLastCheckpoint();

	/** Returns true when the current EMS save slot contains a valid committed checkpoint that has not been superseded by a later actor save. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Autosave and Checkpoints|Checkpoint", meta = (DisplayName = "Has Checkpoint"))
	bool HasCheckpoint();

	/** Returns the latest committed checkpoint record for the current EMS save slot, or an invalid record when none exists/current state superseded it. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Autosave and Checkpoints|Checkpoint", meta = (DisplayName = "Get Current Checkpoint"))
	FEMSCheckpointRecord GetCurrentCheckpoint();

	/**
	 * Adds a named condition that temporarily prevents addon autosaves from starting.
	 * Multiple independent blockers can coexist; use the same name when removing a blocker.
	 */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Autosave", meta = (DisplayName = "Add Autosave Blocker"))
	void AddAutosaveBlocker(FName Blocker);

	/** Removes a previously added autosave blocker. Pending autosaves may start when the final blocker is removed. */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Autosave", meta = (DisplayName = "Remove Autosave Blocker"))
	void RemoveAutosaveBlocker(FName Blocker);

	/** Returns true while at least one named autosave blocker is active. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Autosave and Checkpoints|Autosave", meta = (DisplayName = "Is Autosave Blocked"))
	bool IsAutosaveBlocked() const { return !AutosaveBlockers.IsEmpty(); }

	/** Returns true while an addon-requested EMS save is currently running. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Autosave and Checkpoints|Autosave", meta = (DisplayName = "Is Autosave Active"))
	bool IsAutosaveActive() const { return bSaveActive; }

	/** C++ helper used by checkpoint actors while checkpoint state is being restored. */
	bool IsCheckpointLoadInProgress() const
	{
		return ActiveLoadTask != nullptr || bCheckpointLoadAfterTravel || CheckpointBeingLoaded.IsValid();
	}

	/**
	 * Claims the "place the player here" handoff from a finished checkpoint load.
	 *
	 * A checkpoint whose level or World Partition cell was not loaded when the
	 * load completed missed the On Checkpoint Loaded broadcast, so it asks for the
	 * handoff when it does begin play. Returns true once, for the one checkpoint
	 * the load restored into.
	 */
	bool ConsumeCheckpointPlayerPlacement(const AEMSCheckpoint* Checkpoint);

protected:
	/** The restored checkpoint still owing the player placement, if any. */
	FEMSCheckpointRecord CheckpointAwaitingPlayerPlacement;

	/** The checkpoint a load is currently restoring into. */
	FEMSCheckpointRecord CheckpointBeingLoaded;

	/** Finishes a checkpoint load and hands off the player placement. */
	void CompleteCheckpointLoad();

private:
	UPROPERTY(Transient)
	TObjectPtr<UEMSAsyncSaveGame> ActiveSaveTask;

	UPROPERTY(Transient)
	TObjectPtr<UEMSAsyncLoadGame> ActiveLoadTask;

	TSet<FName> AutosaveBlockers;
	TWeakObjectPtr<AEMSCheckpoint> PendingCheckpointActor;
	TWeakObjectPtr<AEMSCheckpoint> ActiveCheckpointActor;
	FTimerHandle RetryTimer;
	FTimerHandle PeriodicTimer;
	FTimerHandle MapLoadTimer;
	FName ActiveReason;
	FString ActiveSaveSlot;
	FString CheckpointLoadSlot;
	FString AddonSaveSlotAwaitingActorsSaved;
	FEMSCheckpointRecord ActiveCheckpoint;
	FGuid ActiveCheckpointGeneration;
	FGuid ReportedStaleGeneration;
	FEMSAutosavePendingRequest PendingRequest;

	/** Runtime override for the project's periodic interval. Negative means the project setting is used. */
	float PeriodicIntervalOverride = -1.0f;
	double LastSuccessfulSaveSeconds = -DBL_MAX;
	bool bSaveActive = false;
	bool bActiveRequestIsCheckpoint = false;
	bool bCheckpointLoadAfterTravel = false;
	bool bIsShuttingDown = false;
	bool bIgnoreNextSuccessfulActorsSaved = false;

	bool QueueRequest(
		FName Reason,
		const FString& SaveSlot,
		const FEMSCheckpointRecord* CheckpointRecord,
		AEMSCheckpoint* CheckpointActor);

	/** Queues without starting, for callers already inside the start path. */
	bool EnqueueRequest(
		FName Reason,
		const FString& SaveSlot,
		const FEMSCheckpointRecord* CheckpointRecord,
		AEMSCheckpoint* CheckpointActor);

	bool CanStartSave(const FString& RequestedSlot, float& OutRetryDelay) const;
	void TryStartPendingSave();
	void ScheduleRetry(float DelaySeconds);
	void StartSave(
		FName Reason,
		const FString& SaveSlot,
		const FEMSCheckpointRecord* CheckpointRecord,
		AEMSCheckpoint* CheckpointActor);
	void ResetActiveSaveState();
	void CancelPendingRequest(const FString& Message, bool bBroadcastFailure);
	void CancelPendingRequestAfterEMSLoad();
	void ClearCheckpointAfterSupersedingSave(const FString& SaveSlot);
	void RefreshCheckpointActorsFromSaveData();

	/**
	 * Reserves the generation of the EMS save that is about to run, so a commit
	 * that never lands is detectable instead of leaving the previous checkpoint
	 * describing state it was not saved with.
	 */
	bool BeginCheckpointGeneration(const FString& SaveSlot, FGuid& OutGeneration);
	void AbandonCheckpointGeneration(const FString& SaveSlot);
	bool CommitCheckpoint(
		const FEMSCheckpointRecord& Record,
		const FString& SaveSlot,
		const FGuid& Generation);
	UEMSCheckpointSaveGame* ResolveCheckpointSave(const FString& SaveSlot = FString());
	FEMSCheckpointRecord GetCheckpointForSlot(const FString& SaveSlot);
	void PrepareCheckpointActorForLoad();
	void RefreshCheckpointActorAfterLoad(bool bLoadSucceeded);
	void BeginCheckpointLoad();
	void FailCheckpointLoad(const FString& Message);
	void ConfigureWorldTimers(UWorld* World);

	/** Returns the runtime override when one is set, otherwise the project setting. */
	float ResolvePeriodicAutosaveInterval() const;
	void LogMessage(const FString& Message, bool bWarning = false) const;

	UFUNCTION()
	void HandleSaveCompleted();

	UFUNCTION()
	void HandleSaveFailed();

	UFUNCTION()
	void HandleLoadCompleted();

	UFUNCTION()
	void HandleLoadFailed();

	/** Observes normal EMS player loads started outside this addon and drops any pre-load pending autosave. */
	UFUNCTION()
	void HandleEMSPlayerLoaded(const APlayerController* LoadedPlayer);

	/** Observes normal EMS persistent-level loads started outside this addon and drops any pre-load pending autosave. */
	UFUNCTION()
	void HandleEMSLevelLoaded(const TArray<TSoftObjectPtr<AActor>>& LoadedActors);

	/** Observes every normal EMS Save Game Actors completion so later saves can supersede a checkpoint safely. */
	UFUNCTION()
	void HandleEMSActorsSaved(ESaveGameMode Mode, bool bSuccess);

	void HandlePostLoadMap(UWorld* LoadedWorld);
	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	void HandlePeriodicAutosave();
	void HandleMapLoadAutosave();
};
