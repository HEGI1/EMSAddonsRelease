//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAutosaveTypes.h"
#include "GameFramework/Actor.h"
#include "EMSCheckpoint.generated.h"

class APawn;
class APlayerController;
class UBillboardComponent;
class UBoxComponent;
class UEMSAutosaveSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FEMSCheckpointActorActivated);

/** Level-placed checkpoint actor that can activate from player overlap or an explicit Blueprint call. */
UCLASS(Blueprintable, ClassGroup = (EasyMultiSave), meta = (DisplayName = "EMS Checkpoint"))
class EMSADDONSAUTOSAVE_API AEMSCheckpoint : public AActor
{
	GENERATED_BODY()

public:
	AEMSCheckpoint();

	/** Trigger volume used for optional player-overlap activation. Resize it to define the checkpoint area. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Components")
	TObjectPtr<UBoxComponent> Trigger;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> CheckpointSprite;
#endif

	/** Persistent checkpoint identity. IDs must be unique within a world and are generated automatically. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Identity")
	FGuid CheckpointId;

	/** Optional designer-facing label copied into the stored checkpoint record. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EMS Addons|Autosave and Checkpoints|Identity")
	FText DisplayName;

	/** Automatically activates this checkpoint when the local player pawn genuinely enters the Trigger volume after startup/loading. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EMS Addons|Autosave and Checkpoints|Activation")
	bool bActivateOnPlayerOverlap = true;

	/**
	 * Prevents this checkpoint from activating more than once during the current play session unless Reset Checkpoint is called.
	 * When disabled, every accepted re-entry is treated as a fresh checkpoint capture and can save again.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EMS Addons|Autosave and Checkpoints|Activation")
	bool bTriggerOnce = true;

	/** Fired after this actor successfully activates a checkpoint request. */
	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Autosave and Checkpoints|Events")
	FEMSCheckpointActorActivated OnCheckpointActivated;

	/**
	 * Activates this checkpoint explicitly and saves the configured EMS data.
	 * Returns true when the checkpoint system accepts the activation. Trigger Once and pending-state rules still apply.
	 */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Checkpoint", meta = (DisplayName = "Activate Checkpoint"))
	bool ActivateCheckpoint();

	/** Clears this actor's transient activation state and forces its next accepted activation to save again. */
	UFUNCTION(BlueprintCallable, Category = "EMS Addons|Autosave and Checkpoints|Checkpoint", meta = (DisplayName = "Reset Checkpoint"))
	void ResetCheckpoint();

	/** Returns true after this checkpoint has been committed or restored during the current play session. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Autosave and Checkpoints|Checkpoint", meta = (DisplayName = "Was Activated This Session"))
	bool WasActivatedThisSession() const { return bActivatedThisSession; }

	/** Returns true while this checkpoint is waiting for its EMS save/metadata commit to finish. */
	UFUNCTION(BlueprintPure, Category = "EMS Addons|Autosave and Checkpoints|Checkpoint", meta = (DisplayName = "Is Activation Pending"))
	bool IsActivationPending() const { return bActivationPending; }

	/**
	 * Returns true when no other EMS Checkpoint in this world uses the same
	 * persistent ID.
	 *
	 * Authoring validation, reported through the Begin Play log rather than a
	 * Blueprint node. It iterates every checkpoint in the world, which is not a
	 * cost gameplay should be able to pay per frame.
	 */
	bool HasUniqueCheckpointId() const;

#if WITH_DEV_AUTOMATION_TESTS
	/** Drives the same restore path a completed checkpoint load drives. */
	void SimulateCheckpointRestoreForTesting(const FEMSCheckpointRecord& Checkpoint) { HandleCheckpointLoaded(Checkpoint); }

	/** True while a restored player still has to be placed at this checkpoint. */
	bool IsPlayerPlacementPendingForTesting() const { return bRestorePlayerToCenterOnArm; }

	bool PlaceRestoredPlayerForTesting(APawn* PlayerPawn) { return PlaceRestoredPlayer(PlayerPawn); }
#endif

#if WITH_EDITOR
	/** Generates a new persistent checkpoint ID. Use this to resolve an intentionally duplicated ID. */
	UFUNCTION(CallInEditor, Category = "EMS Addons|Autosave and Checkpoints|Identity", meta = (DisplayName = "Regenerate Checkpoint ID"))
	void RegenerateCheckpointId();

	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostDuplicate(EDuplicateMode::Type DuplicateMode) override;
#endif

	virtual void PostActorCreated() override;
	virtual void PostLoad() override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	friend class UEMSAutosaveSubsystem;

	UPROPERTY(Transient)
	bool bActivatedThisSession = false;

	UPROPERTY(Transient)
	bool bActivationPending = false;

	UPROPERTY(Transient)
	bool bForceSaveNextActivation = false;

	/** True while collision is deliberately disabled so EMS restoration cannot activate the checkpoint. */
	UPROPERTY(Transient)
	bool bWaitingForCheckpointLoad = false;

	/** One-shot request to place the restored local player at this checkpoint's trigger center before overlap is rearmed. */
	UPROPERTY(Transient)
	bool bRestorePlayerToCenterOnArm = false;

	/** Short-lived fallbacks used while EMS/player startup state is settling. */
	bool bLoadWatchScheduled = false;
	bool bArmWatchScheduled = false;
	FTimerHandle CheckpointMetadataWatchTimer;
	ECollisionEnabled::Type SavedTriggerCollision = ECollisionEnabled::QueryOnly;

	void EnsureCheckpointId();
	void MarkActivationPending();
	void MarkActivationCommitted();
	void MarkActivationFailed();
	bool MatchesCheckpointRecord(const FEMSCheckpointRecord& Checkpoint) const;
	void DisableTriggerForCheckpointLoad();
	void RestoreTriggerCollision();
	void ArmTriggerWhenPlayerReady();
	bool PlaceRestoredPlayer(APawn* PlayerPawn);
	void ResolveArmWatch();
	void RefreshTriggerCollisionFromSaveData(UEMSAutosaveSubsystem* Autosave);
	void ScheduleLoadWatch();
	void ResolveLoadWatch();
	void ScheduleCheckpointMetadataWatch();
	void ResolveCheckpointMetadataWatch();

	/** Called by EMS after normal player state, including pawn transform, has been applied. */
	UFUNCTION()
	void HandleEMSPlayerLoaded(const APlayerController* LoadedPlayer);

	UFUNCTION()
	void HandleCheckpointLoaded(FEMSCheckpointRecord Checkpoint);

	UFUNCTION()
	void HandleCheckpointLoadFailed(FEMSCheckpointRecord Checkpoint);

	UFUNCTION()
	void HandleTriggerBeginOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult);
};
