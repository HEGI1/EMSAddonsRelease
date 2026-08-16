//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSActorSaveInterface.h"
#include "EMSAddonRestoreParticipant.h"
#include "EMSAddonsTypes.h"
#include "EMSLevelSequenceTypes.h"
#include "LevelSequenceActor.h"
#include "TimerManager.h"
#include "EMSLevelSequenceActor.generated.h"

class UBillboardComponent;
class UEMSObject;
class ULevelSequencePlayer;
class UMovieSceneSequencePlayer;
class USceneComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FEMSLevelSequenceRestoreStarted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FEMSLevelSequenceRestoreFinished,
	FEMSAddonResult,
	Result);

UCLASS(
	Blueprintable,
	ClassGroup = (EasyMultiSave),
	meta = (DisplayName = "EMS Level Sequence Actor"))
class EMSADDONSLEVELSEQUENCE_API AEMSLevelSequenceActor
	: public ALevelSequenceActor
	, public IEMSActorSaveInterface
	, public IEMSAddonRestoreParticipant
{
	GENERATED_BODY()

public:
	AEMSLevelSequenceActor(const FObjectInitializer& ObjectInitializer);

#if WITH_EDITORONLY_DATA
	/**
	 * Editor-only marker. Level Sequence Actors carry no geometry, so without a
	 * root and this sprite they are only reachable from the World Outliner.
	 */
	UPROPERTY()
	TObjectPtr<UBillboardComponent> SequenceSprite;
#endif

	UPROPERTY(SaveGame, VisibleInstanceOnly, Category = "EMS Addons|Level Sequence|Advanced", meta = (AdvancedDisplay))
	FEMSLevelSequencePlaybackState PersistedState;

	/**
	 * Leaves a sequence that was saved while playing paused at its restored
	 * position, for cutscenes that gameplay starts rather than the load.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EMS Addons|Level Sequence|Restore")
	bool bResumeIfPlaying = true;

	/** Whether the frames between the runtime and saved position are evaluated. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EMS Addons|Level Sequence|Restore")
	EEMSLevelSequencePositionRestorePolicy PositionRestorePolicy =
		EEMSLevelSequencePositionRestorePolicy::JumpAndEvaluate;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EMS Addons|Level Sequence|Restore", meta = (ClampMin = "0.05", ClampMax = "60.0", Units = "s"))
	float RestoreTimeout = 5.0f;

	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Level Sequence|Events")
	FEMSLevelSequenceRestoreStarted OnLevelSequenceRestoreStarted;

	UPROPERTY(BlueprintAssignable, Category = "EMS Addons|Level Sequence|Events")
	FEMSLevelSequenceRestoreFinished OnLevelSequenceRestoreFinished;

	/**
	 * Restore status for C++ callers.
	 *
	 * Blueprint reads the same values through EMS Addon Restore Participant,
	 * so these are deliberately not exposed a second time.
	 */
	bool IsLevelSequenceRestorePending() const;
	FEMSAddonResult GetLevelSequenceRestoreResult() const;

	virtual void ActorPreSave_Implementation() override;
	virtual void ActorPreLoad_Implementation() override;
	virtual void ActorLoaded_Implementation() override;

	virtual bool IsEMSAddonRestoreComplete() const override;
	virtual FEMSAddonResult GetEMSAddonRestoreResult() const override;

protected:
	bool CaptureLevelSequenceState();
	bool RestoreLevelSequenceState();
	bool ValidateLevelSequenceState(FText& OutReason) const;
	EEMSAddonRestorePhase GetLevelSequenceRestorePhase() const;

	/** Isolated so automation can exercise every net mode from one editor world. */
	virtual ENetMode GetLevelSequenceNetMode() const
	{
		return GetNetMode();
	}

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

	virtual ULevelSequencePlayer* GetPersistenceSequencePlayer() const;
	virtual bool IsLevelSequenceRuntimeReadyForRestore(
		bool& bOutWaitingForBindings) const;
	virtual bool IsRelevantEMSLoadActive() const;
	void InitializeAutoPlayGateForBeginPlay();
	void HandleInitialAutoPlay();

private:
	/**
	 * Stall guard on the wait for EMS to decide whether a restore is coming.
	 *
	 * Not a designer setting. The wait is normally over in well under a second,
	 * but a World Partition level reports a load as active until EMS finishes
	 * its initial polling pass, and EMS guards that loop against hanging as
	 * well. Without a bound here an authored autoplay would never start.
	 * Generous on purpose: a restore that arrives late still wins, because
	 * ActorPreLoad stops the player again.
	 */
	static constexpr double MaxInitialAutoPlayWaitSeconds = 30.0;

	FTimerHandle RestoreRetryTimer;
	FTimerHandle InitialAutoPlayTimer;
	FEMSAddonResult LastRestoreResult;

	UPROPERTY(Transient)
	TObjectPtr<UEMSObject> BoundEMSObject;

	UPROPERTY(Transient)
	TObjectPtr<ULevelSequencePlayer> BoundSequencePlayer;

	EEMSAddonRestorePhase RestorePhase = EEMSAddonRestorePhase::Idle;

	double RestoreStartSeconds = 0.0;
	double InitialAutoPlayWaitStartSeconds = 0.0;
	double InitialAutoPlayStartSeconds = 0.0;
	int32 RuntimeCompletedLoops = 0;
	bool bRuntimeHasStarted = false;
	bool bRuntimeFinished = false;
	bool bAuthoredAutoPlay = false;
	bool bAutoPlaySuppressed = false;
	bool bInitialAutoPlayDecisionPending = false;
	bool bInitialAutoPlayDecisionFinal = false;
	bool bRestoreRequested = false;
	bool bLevelLoadCompletionReceived = false;
	bool bRestoreClockStarted = false;
	bool bApplyingRestore = false;
	bool bIsEndingPlay = false;

	/**
	 * The clock every deferred wait in this class is measured against.
	 *
	 * World time rather than wall clock, because the waits retry on tick timers
	 * and both stop together when the game is paused. Wall clock would keep
	 * burning the budget while nothing could make progress.
	 */
	double GetDeferralClockSeconds() const;

	void SkipPersistenceWithoutAuthority();
	bool CanAccessLevelSequence(FText& OutReason, bool bRequirePlayer) const;
	bool ValidateStateInternal(FText& OutReason, bool bRequirePlayer) const;
	bool ApplyPersistedState(FText& OutReason);
	bool AreRootPossessableBindingsReady() const;
	void QueueRestoreForEMSCompletion();
	void StartDeferredRestore();
	void AttemptRestore();
	void ScheduleRestoreRetry();
	void FinalizeRestore();
	void FailRestore(const FText& Reason);
	void CancelPendingRestore(bool bReportCanceled);
	void BindEMSCompletionDelegates();
	void UnbindEMSCompletionDelegates();
	void BindSequencePlayerDelegates();
	void UnbindSequencePlayerDelegates();
	void SuppressAutoPlay();
	void ReleaseAutoPlaySuppression();
	void ScheduleInitialAutoPlay();
	void StartAuthoredAutoPlayWhenReady();
	void LogRestoreIssue(const FText& Reason) const;

	UFUNCTION()
	void HandleEMSLevelLoaded(
		const TArray<TSoftObjectPtr<AActor>>& LoadedActors);

	UFUNCTION()
	void HandleSequencePlayed();

	UFUNCTION()
	void HandleSequencePlayedReverse();

	UFUNCTION()
	void HandleSequencePaused();

	UFUNCTION()
	void HandleSequenceStopped();

	UFUNCTION()
	void HandleSequenceFinished();

	void HandleSequenceUpdated(
		const UMovieSceneSequencePlayer& Player,
		FFrameTime CurrentTime,
		FFrameTime PreviousTime);
};
