//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAutosaveRequest.h"
#include "EMSAutosaveTypes.h"
#include "EMSTypes.h"
#include "Engine/EngineBaseTypes.h"
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
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMSSaveSlotEvent, const FString&, SlotName);

/** Why the addon switches the current EMS slot. Decides how the running streaming session is treated. */
enum class EEMSSlotSwitch : uint8
{
	/** Into a slot the save is about to write. The session continues there. */
	ContinueSession,
	/** Back to the slot the game was using, after a failed or abandoned save. */
	Return,
	/** Back to the previous slot while the world is going away. */
	LeaveWorld,
};

/**
 * Game-instance service for autosaves, manual saves and checkpoints.
 *
 * Every save is an ordinary EMS slot of the current profile: the current EMS slot
 * is switched to the target slot and a normal EMS save runs. The newest save stays
 * the current slot, so "Continue" is a normal EMS load.
 *
 * - Autosaves: "<Profile>" (Autosave Slot Count 0) or "<profile>_autosave_<0..N-1>", oldest replaced first.
 * - Manual saves: "<profile>_manual_<name>".
 * - Checkpoints: "<profile>_checkpoint" or "<profile>_checkpoint_<name>", named on the checkpoint actor.
 * Generated IDs are lowercase; existing saves retain their original spelling.
 *
 * EMS stores the current slot in its user config as soon as it changes. While a save runs, the slot it
 * writes and the slot it left are kept in the same config, so a game that ends during the save continues
 * from the last complete save on its next start (see Recover Interrupted Save).
 */
UCLASS(config = EmsUser, configdonotcheckdefaults)
class EMSADDONSAUTOSAVE_API UEMSAutosaveSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Fired immediately before an addon-requested EMS save starts. Reason identifies the request source (Manual for Manual Save). */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSAutosaveEvent OnAutosaveStarted;

	/** Fired after an addon-requested EMS save finishes successfully. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSAutosaveEvent OnAutosaveCompleted;

	/** Fired when an addon-requested save cannot be completed. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSAutosaveEvent OnAutosaveFailed;

	/** Fired when a checkpoint activation is accepted by the subsystem. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSCheckpointEvent OnCheckpointActivated;

	/** Fired after the checkpoint's EMS save and its checkpoint record were written. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSCheckpointEvent OnCheckpointCommitted;

	/** Fired after the latest checkpoint has finished loading through EMS. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSCheckpointEvent OnCheckpointLoaded;

	/** Fired when loading the latest checkpoint fails or is canceled. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSCheckpointEvent OnCheckpointLoadFailed;

	/** Fired after Load Save opened the slot's map and EMS finished loading it. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSSaveSlotEvent OnSaveLoaded;

	/** Fired when a Load Save that was started could not finish. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSSaveSlotEvent OnSaveLoadFailed;

	/**
	 * Queues an autosave of the configured EMS data into the next autosave slot of the current profile.
	 * Display Name overrides the current autosave display name for this request. Empty uses that current name.
	 * The name is captured when requested, including while blocked. Returns false on clients or while EMS is loading.
	 */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Autosave", meta = (DisplayName = "Request Autosave"))
	bool RequestAutosave(FName Reason = NAME_None, FText DisplayName = FText());

	/** Sets the location/name captured by future autosaves, including periodic and map-leave saves. Session only. */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Autosave")
	void SetAutosaveDisplayName(FText DisplayName) { AutosaveDisplayName = DisplayName; }

	UFUNCTION(BlueprintPure, Category = "EMS Addons|Autosave and Checkpoints|Autosave")
	FText GetAutosaveDisplayName() const { return AutosaveDisplayName; }

	/**
	 * Saves the configured EMS data into lowercase "<profile>_manual_<savename>" now, replacing an earlier save of that name.
	 * Enable Autosave, blockers and the minimum interval do not apply. Returns false when EMS or another addon
	 * save is busy, or level streaming is not ready; the result is reported through the autosave events with Reason "Manual".
	 * Display Name is presentation only and defaults to SaveName. SaveName still determines which slot is replaced.
	 */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Saves", meta = (DisplayName = "Manual Save"))
	bool ManualSave(const FString& SaveName, FText DisplayName = FText());

	/**
	 * Loads any existing EMS slot, for example one from Get Save Slots.
	 * Makes it the current EMS slot, opens its map (also when it is the current map, so the load starts
	 * from a fresh world) and loads the configured data with EMS. Unsaved progress is discarded.
	 */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Saves", meta = (DisplayName = "Load Save"))
	bool LoadSave(const FString& SlotName);

	/** Returns true from a successful Load Save call until On Save Loaded or On Save Load Failed. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Autosave and Checkpoints|Saves", meta = (DisplayName = "Is Save Load In Progress"))
	bool IsSaveLoadInProgress() const
	{
		return bSaveLoadAfterTravel || !SaveBeingLoaded.IsEmpty();
	}

	/** Returns the current profile's existing slots, newest first, excluding targets known to be incomplete this session. */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Saves", meta = (DisplayName = "Get Save Slots"))
	TArray<FString> GetSaveSlots();

	/** Returns the current profile's saves, newest first, with names suitable for a save menu. */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Saves")
	TArray<FEMSSaveSlotDetails> GetSaveSlotsWithDetails();

	/** Reads an existing save without switching the current slot. Old saves receive a fallback name. */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Saves")
	bool GetSaveSlotDetails(const FString& SlotName, FEMSSaveSlotDetails& Details);

	/** Renames an existing save's display label only. Empty clears the alias and uses a fallback name. Refuses while busy. */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Saves")
	bool SetSaveSlotDisplayName(const FString& SlotName, FText DisplayName);

	/** Returns the profile of the current EMS slot. Saves of one profile share it as their name prefix. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Autosave and Checkpoints|Saves", meta = (DisplayName = "Get Save Profile"))
	FString GetSaveProfile() const;

	/** Returns the profile an EMS slot belongs to. A slot the addon did not name is its own profile. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Autosave and Checkpoints|Saves", meta = (DisplayName = "Get Slot Profile"))
	static FString GetSlotProfile(const FString& SlotName);

	/**
	 * Adds a slot-bound custom save file that every addon save carries from the previous slot into the new one.
	 * Project Settings has the permanent list; this adds to it for the current session.
	 */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Saves", meta = (DisplayName = "Add Carried Custom File"))
	void AddCarriedCustomFile(const FString& FileName);

	/** Removes a custom save file added with Add Carried Custom File. */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Saves", meta = (DisplayName = "Remove Carried Custom File"))
	void RemoveCarriedCustomFile(const FString& FileName);

	/** Lowercase ID of autosave slot Index (0-based) of a profile, as EMS stores it. */
	static FString MakeAutosaveSlotName(const FString& Profile, int32 Index);

	/** Lowercase ID of a manual save slot of a profile, as EMS stores it. */
	static FString MakeManualSlotName(const FString& Profile, const FString& SaveName);

	/** Lowercase ID of a checkpoint slot of a profile. An empty name gives "<profile>_checkpoint". */
	static FString MakeCheckpointSlotName(const FString& Profile, const FString& CheckpointSlotName);

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
	 * Loads the checkpoint that belongs to the current progress.
	 * Makes its checkpoint slot the current EMS slot, travels to the checkpoint world when required and lets EMS
	 * restore the saved player/level state. After EMS finishes, the local player is placed at the matching
	 * checkpoint trigger center while keeping the EMS-restored rotation.
	 */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Checkpoint", meta = (DisplayName = "Load Last Checkpoint"))
	bool LoadLastCheckpoint();

	/** Returns true when the current progress has a checkpoint. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Autosave and Checkpoints|Checkpoint", meta = (DisplayName = "Has Checkpoint"))
	bool HasCheckpoint();

	/**
	 * Returns the checkpoint of the current progress: the last one activated before the current EMS slot was saved.
	 * Later autosaves and manual saves carry it along, so loading any save returns to that save's checkpoint.
	 */
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

	/**
	 * Internal C++ reservation used by the pre-load-map synchronous save path.
	 * Switches the current EMS slot to the next autosave slot of CurrentSlot's profile and returns it in OutTargetSlot.
	 */
	bool BeginSynchronousAutosave(FName Reason, const FString& CurrentSlot, FString& OutTargetSlot);

	/** Completes a synchronous save reservation and emits the normal addon and EMS callbacks. Returns the validated save result. */
	bool CompleteSynchronousAutosave(bool bSuccess, int32 SaveDataFlags);

	/** Aborts a synchronous save reservation before any EMS files are written. */
	void AbortSynchronousAutosave();

	/** C++ helper used by checkpoint actors while checkpoint state is being restored. */
	bool IsCheckpointLoadInProgress() const
	{
		// A Load Save task is an ordinary EMS load for checkpoint actors, not a checkpoint load.
		return bCheckpointLoadAfterTravel
			|| CheckpointBeingLoaded.IsValid()
			|| (ActiveLoadTask != nullptr && SaveBeingLoaded.IsEmpty());
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

	/**
	 * Recovers from a game that ended while an addon save was writing (crash, power loss, forced quit).
	 * When the current EMS slot is still the slot that save was writing, the previous slot becomes current
	 * again and the incomplete slot is deleted. Runs automatically once per process when the first game
	 * instance starts. Returns true when it recovered a save.
	 */
	bool RecoverInterruptedSave();

protected:
	/** The restored checkpoint still owing the player placement, if any. */
	FEMSCheckpointRecord CheckpointAwaitingPlayerPlacement;

	/** The checkpoint a load is currently restoring into. */
	FEMSCheckpointRecord CheckpointBeingLoaded;

	/** Finishes a checkpoint load and hands off the player placement. */
	void CompleteCheckpointLoad();

	/** The slot the next autosave of Profile writes: the first unused autosave slot, otherwise the oldest. */
	FString ResolveAutosaveTarget(const FString& Profile);

	/**
	 * Makes TargetSlot the current EMS slot. For ContinueSession in a World Partition or level streaming
	 * session, the stale Level data of an existing TargetSlot is removed first: EMS 1.86 keeps restoring
	 * streamed cells from memory only when switching to a slot without Level data. bOutRemovedLevelData
	 * reports that removal, after which TargetSlot no longer holds a loadable save until the save writes it.
	 * Returns false when EMS refused the switch, for example because an EMS task is active.
	 */
	bool SwitchToSaveSlot(const FString& TargetSlot, EEMSSlotSwitch Switch, bool* bOutRemovedLevelData = nullptr);

	/**
	 * Copies the carried custom files (and, unless excluded, the checkpoint record) from SourceSlot into
	 * TargetSlot with EMS Copy Save Slot, and removes carried files the source does not have, so the
	 * target never keeps stale ones. A checkpoint save excludes the record: it writes its own after success.
	 */
	bool CarryCustomFiles(const FString& SourceSlot, const FString& TargetSlot, bool bIncludeCheckpointRecord = true);

	/**
	 * Clears the checkpoint record in Slot when it names Slot itself, i.e. Slot holds that checkpoint's save.
	 * Called before a save overwrites Slot, so an interrupted overwrite never leaves a record pointing at
	 * partly written data. Returns false only when the clear could not be written.
	 */
	bool InvalidateCheckpointStoredIn(const FString& Slot, bool bRefreshActors);

	void StartSave(
		FName Reason,
		const FString& Profile,
		const FString& TargetSlot,
		const FEMSCheckpointRecord* CheckpointRecord,
		AEMSCheckpoint* CheckpointActor,
		const FText& DisplayName = FText(),
		EEMSSaveSlotType SaveType = EEMSSaveSlotType::Autosave);

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
	FText AutosaveDisplayName;
	FText ActiveDisplayName;
	EEMSSaveSlotType ActiveSaveType = EEMSSaveSlotType::Autosave;
	/** The slot the active save writes. */
	FString ActiveSaveSlot;
	FString ActiveSaveUser;
	double ActiveSlotInfoWriteStartSeconds = DBL_MAX;
	/** Incomplete targets from failed preparation or saving. Scoped to EMS user and this game instance. */
	TMap<FString, TSet<FString>> UnavailableSlots;
	/** The current slot before the active save switched away from it. A save that fails returns to it. */
	FString ActivePreviousSlot;
	FString CheckpointLoadSlot;
	FString AddonSaveSlotAwaitingActorsSaved;
	FEMSCheckpointRecord ActiveCheckpoint;
	FEMSAutosavePendingRequest PendingRequest;

	/** Custom files added at runtime with Add Carried Custom File. */
	TArray<FString> RuntimeCarriedCustomFiles;

	/** Runtime override for the project's periodic interval. Negative means the project setting is used. */
	float PeriodicIntervalOverride = -1.0f;
	double LastSuccessfulSaveSeconds = -DBL_MAX;
	bool bSaveActive = false;
	bool bActiveRequestIsCheckpoint = false;
	bool bCheckpointLoadAfterTravel = false;
	bool bIsShuttingDown = false;
	bool bIgnoreNextSuccessfulActorsSaved = false;
	bool bWarnedSessionClearingSwitch = false;

	/** Slot that Load Save is loading, until its load finishes. */
	FString SaveBeingLoaded;
	/** Level the loading slot was saved in, as EMS names it. */
	FName SaveLoadLevel;
	/** A load the game itself started on arrival, which Load Save reports the result of. */
	TWeakObjectPtr<UEMSAsyncLoadGame> AdoptedLoadTask;
	bool bSaveLoadAfterTravel = false;

	/** The current slot before Load Save or Load Last Checkpoint switched away from it. Failed travel returns to it. */
	FString SlotBeforeTravelLoad;

	/** Save journal, stored with the EMS current slot. Set while an addon save may have written TargetSlot. */
	UPROPERTY(config)
	FString InterruptedSaveUser;

	UPROPERTY(config)
	FString InterruptedSaveSlot;

	UPROPERTY(config)
	FString InterruptedSavePreviousSlot;

	/** Records that an addon save is about to make TargetSlot current and write it. */
	void BeginSaveJournal(const FString& TargetSlot, const FString& PreviousSlot);
	/** The save ended (completed, failed or abandoned), so nothing on disk is left half written by it. */
	void ClearSaveJournal();
	/** Clears a journal left by a cut-off save once a successful EMS save of all addon data rewrote its slot. */
	void ClearJournalCompletedBy(const FString& SavedSlot, ESaveGameMode Mode);
	/** Makes SlotBeforeTravelLoad current again when a load never started in the old world (failed travel, or no load task). */
	void ReturnFromFailedTravel(const FString& LoadingSlot);
	void HandleTravelFailure(UWorld* World, ETravelFailure::Type FailureType, const FString& ErrorString);

	bool QueueRequest(
		FName Reason,
		const FString& Profile,
		const FEMSCheckpointRecord* CheckpointRecord,
		AEMSCheckpoint* CheckpointActor,
		const FText& DisplayName = FText());

	/** Queues without starting, for callers already inside the start path. */
	bool EnqueueRequest(
		FName Reason,
		const FString& Profile,
		const FEMSCheckpointRecord* CheckpointRecord,
		AEMSCheckpoint* CheckpointActor,
		const FText& DisplayName = FText());

	bool WriteSaveSlotMetadata(const FString& SlotName, const FText& DisplayName, EEMSSaveSlotType SaveType);
	bool HasWrittenActiveSlotInfo() const;
	bool DoesSaveSlotRemain(const FString& SlotName) const;
	bool IsSlotUnavailable(const FString& SlotName) const;
	void SetSlotUnavailable(const FString& SlotName, bool bUnavailable);
	bool PreflightCarriedCustomFiles(const FString& SourceSlot, const FString& TargetSlot, bool bIncludeCheckpointRecord) const;
	/** Resolves a generated ID to the original spelling of an existing slot, for pre-lowercase saves. */
	FString ResolveSaveSlotTarget(const FString& SlotName) const;

	bool CanStartSave(const FString& RequestedProfile, float& OutRetryDelay) const;
	bool IsLevelStreamingBlockingSave() const;
	void TryStartPendingSave();
	void ScheduleRetry(float DelaySeconds);
	/** Ends the active save. Keeps the save journal when the game is still on a slot that save left incomplete. */
	void ResetActiveSaveState(bool bClearJournal = true);
	/**
	 * Makes the slot from before a failed or abandoned save current again.
	 * Returns false only when the game is still on FailedSlot because EMS refused the switch (an EMS task is running).
	 * With World Partition or level streaming, EMS stops restoring streamed cells from memory until the next load.
	 */
	bool ReturnToPreviousSlot(const FString& PreviousSlot, const FString& FailedSlot, EEMSSlotSwitch Switch = EEMSSlotSwitch::Return);
	/**
	 * Leaves a save that was abandoned before EMS wrote anything. A target whose Level data was removed
	 * for the switch is no longer a loadable save and is deleted like a failed one; otherwise it stays.
	 */
	bool ReturnFromAbandonedSave(const FString& PreviousSlot, const FString& AbandonedSlot, bool bRemovedLevelData);
	/**
	 * ReturnToPreviousSlot for a save that EMS reported as failed after it may have
	 * started writing, then deletes FailedSlot. If deletion fails, excludes it from addon lists and loads for this session.
	 * Only for a reported failure: a save whose result is unknown may be complete.
	 * Same return value as ReturnToPreviousSlot.
	 */
	bool ReturnAndDiscardFailedSave(const FString& PreviousSlot, const FString& FailedSlot);
	void CancelPendingRequest(const FString& Message, bool bBroadcastFailure);
	void CancelPendingRequestAfterEMSLoad();
	void RefreshCheckpointActorsFromSaveData();
	TArray<FString> GetCarriedCustomFiles(bool bIncludeCheckpointRecord = true) const;

	bool CommitCheckpoint(const FEMSCheckpointRecord& Record, const FString& SaveSlot);

	/** Rewrites a 1.1 checkpoint record in Slot as a version 2 record naming Slot, before it is carried elsewhere. */
	bool UpgradeLegacyCheckpointRecord(const FString& Slot);

	/**
	 * A game's own EMS save that wrote the slot holding a checkpoint replaced that checkpoint's
	 * data. The record is cleared, as in 1.1. Addon saves clear it before they write.
	 */
	void ClearCheckpointOverwrittenBy(const FString& SavedSlot);
	UEMSCheckpointSaveGame* ResolveCheckpointSave(const FString& SaveSlot = FString());
	FEMSCheckpointRecord GetCheckpointForSlot(const FString& SaveSlot);
	void PrepareCheckpointActorForLoad();
	void RefreshCheckpointActorAfterLoad(bool bLoadSucceeded);
	void BeginCheckpointLoad();
	void FailCheckpointLoad(const FString& Message);
	void ConfigureWorldTimers(UWorld* World);
	void BeginSaveLoad(UWorld* LoadedWorld);
	/** Reports the result of the one EMS load already running on arrival, when it loads the configured data. */
	bool AdoptRunningLoad();
	void CompleteSaveLoad();
	void FailSaveLoad(const FString& Message);
	void ResetSaveLoadState();

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

	/** Observes every normal EMS Save Game Actors completion. */
	UFUNCTION()
	void HandleEMSActorsSaved(ESaveGameMode Mode, bool bSuccess);

	void HandlePostLoadMap(UWorld* LoadedWorld);
	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	void HandlePeriodicAutosave();
	void HandleMapLoadAutosave();
};
