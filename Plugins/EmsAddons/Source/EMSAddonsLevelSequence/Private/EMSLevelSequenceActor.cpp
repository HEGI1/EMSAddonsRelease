//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSLevelSequenceActor.h"

#include "Components/BillboardComponent.h"
#include "Components/SceneComponent.h"
#include "EMSAddonsAuthority.h"
#include "EMSAddonsGameThread.h"
#include "EMSAddonsLevelSequence.h"
#include "EMSMisc.h"
#include "EMSObject.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "LevelSequence.h"
#include "LevelSequencePlayer.h"
#include "MovieScene.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSequencePlayer.h"
#include "UObject/ConstructorHelpers.h"

#define LOCTEXT_NAMESPACE "EMSLevelSequenceActor"

AEMSLevelSequenceActor::AEMSLevelSequenceActor(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;
	LastRestoreResult = FEMSAddonResult::Success();

	if (!GetRootComponent())
	{
		SetRootComponent(
			CreateDefaultSubobject<USceneComponent>(TEXT("SequenceRoot")));
	}

#if WITH_EDITORONLY_DATA
	SequenceSprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(
		TEXT("SequenceSprite"));
	if (SequenceSprite)
	{
		static ConstructorHelpers::FObjectFinder<UTexture2D> SequenceSpriteTexture(
			TEXT("/Engine/EditorResources/S_LevelSequence"));
		if (SequenceSpriteTexture.Succeeded())
		{
			SequenceSprite->SetSprite(SequenceSpriteTexture.Object);
		}

		SequenceSprite->SpriteInfo.Category = TEXT("EasyMultiSave");
		SequenceSprite->SpriteInfo.DisplayName =
			LOCTEXT("EMSSpriteCategory", "Easy Multi Save");
		SequenceSprite->bIsScreenSizeScaled = true;
		SequenceSprite->SetupAttachment(GetRootComponent());
	}
#endif
}

void AEMSLevelSequenceActor::BeginPlay()
{
	// A sublevel that is hidden and shown again routes End Play and then Begin
	// Play on the same actor, so this has to clear or the actor can never capture
	// or restore again for the rest of the session.
	bIsEndingPlay = false;

	InitializeAutoPlayGateForBeginPlay();

	Super::BeginPlay();

	BindSequencePlayerDelegates();
	BindEMSCompletionDelegates();
	ScheduleInitialAutoPlay();
}

void AEMSLevelSequenceActor::InitializeAutoPlayGateForBeginPlay()
{
	bAuthoredAutoPlay = PlaybackSettings.bAutoPlay;
	PlaybackSettings.bAutoPlay = false;
	bAutoPlaySuppressed = true;
	bInitialAutoPlayDecisionPending = true;
	bInitialAutoPlayDecisionFinal = false;
	InitialAutoPlayWaitStartSeconds = GetDeferralClockSeconds();
}

double AEMSLevelSequenceActor::GetDeferralClockSeconds() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() : 0.0;
}

void AEMSLevelSequenceActor::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	bIsEndingPlay = true;
	CancelPendingRestore(true);

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(InitialAutoPlayTimer);
	}
	bInitialAutoPlayDecisionPending = false;

	// The next Begin Play reads PlaybackSettings.bAutoPlay back as the authored
	// setting, and a hidden sublevel shows again on the same actor. Leaving the
	// suppression placeholder in it would make that read record the placeholder
	// as the designer's choice and turn autoplay off for good.
	ReleaseAutoPlaySuppression();

	// This request belongs to the lifecycle that is ending. Left set, it makes
	// the next Begin Play skip scheduling its autoplay gate at all.
	bRestoreRequested = false;
	bLevelLoadCompletionReceived = false;

	UnbindEMSCompletionDelegates();
	UnbindSequencePlayerDelegates();
	Super::EndPlay(EndPlayReason);
}

void AEMSLevelSequenceActor::Destroyed()
{
	bIsEndingPlay = true;
	CancelPendingRestore(true);
	UnbindEMSCompletionDelegates();
	UnbindSequencePlayerDelegates();
	Super::Destroyed();
}

bool AEMSLevelSequenceActor::CanAccessLevelSequence(
	FText& OutReason,
	const bool bRequirePlayer) const
{
	if (!IsInGameThread())
	{
		OutReason = LOCTEXT(
			"LevelSequenceGameThreadOnly",
			"Level Sequence save state must be accessed on the game thread.");
		return false;
	}

	if (HasAnyFlags(RF_ClassDefaultObject) || bIsEndingPlay)
	{
		OutReason = LOCTEXT(
			"LevelSequenceUnavailable",
			"The Level Sequence Actor is unavailable or ending play.");
		return false;
	}

	if (!EMSAddons::HasPersistenceAuthority(this, GetLevelSequenceNetMode()))
	{
		OutReason = LOCTEXT(
			"LevelSequenceAuthorityOnly",
			"Only the authoritative Level Sequence Actor may capture or restore persistent playback state. Persistent state is server-authoritative and is never captured or restored on a client.");
		return false;
	}

	if (!GetSequence())
	{
		OutReason = LOCTEXT(
			"LevelSequenceMissingAsset",
			"The actor has no Level Sequence asset.");
		return false;
	}

	if (bRequirePlayer)
	{
		const ULevelSequencePlayer* Player = GetPersistenceSequencePlayer();
		if (!Player || !Player->IsValid() || Player->GetSequence() != GetSequence())
		{
			OutReason = LOCTEXT(
				"LevelSequenceMissingPlayer",
				"The Level Sequence player is not initialized for the assigned asset.");
			return false;
		}
	}

	OutReason = FText::GetEmpty();
	return true;
}

bool AEMSLevelSequenceActor::CaptureLevelSequenceState()
{
	FText Reason;
	if (!CanAccessLevelSequence(Reason, true))
	{
		LogRestoreIssue(Reason);
		return false;
	}

	BindSequencePlayerDelegates();
	ULevelSequencePlayer* Player = GetPersistenceSequencePlayer();
	if (!Player)
	{
		return false;
	}

	const FQualifiedFrameTime CurrentTime = Player->GetCurrentTime();
	if (!CurrentTime.Rate.IsValid()
		|| !FMath::IsFinite(CurrentTime.Time.GetSubFrame())
		|| !FMath::IsFinite(Player->GetPlayRate()))
	{
		LogRestoreIssue(LOCTEXT(
			"LevelSequenceInvalidRuntimeState",
			"The Level Sequence player returned an invalid time or play rate."));
		return false;
	}

	FEMSLevelSequencePlaybackState CapturedState;
	CapturedState.bHasValidData = true;
	CapturedState.SetPosition(CurrentTime.Time);
	CapturedState.SetTickResolution(CurrentTime.Rate);
	CapturedState.PlayRate = Player->GetPlayRate();
	CapturedState.bReversePlayback =
		Player->IsReversed() || CapturedState.PlayRate < 0.0f;
	CapturedState.CompletedLoops = FMath::Max(0, RuntimeCompletedLoops);
	CapturedState.SequenceAsset = FSoftObjectPath(GetSequence());

	if (bRuntimeFinished)
	{
		CapturedState.PlaybackStatus =
			EEMSLevelSequencePlaybackStatus::Finished;
	}
	else if (Player->IsPlaying())
	{
		CapturedState.PlaybackStatus =
			EEMSLevelSequencePlaybackStatus::Playing;
	}
	else if (Player->IsPaused())
	{
		CapturedState.PlaybackStatus =
			EEMSLevelSequencePlaybackStatus::Paused;
	}
	else
	{
		CapturedState.PlaybackStatus =
			EEMSLevelSequencePlaybackStatus::Stopped;
	}

	CapturedState.bHasStarted =
		bRuntimeHasStarted
		|| CapturedState.PlaybackStatus
			!= EEMSLevelSequencePlaybackStatus::Stopped;

	PersistedState = MoveTemp(CapturedState);
	LastRestoreResult = FEMSAddonResult::Success();
	return true;
}

bool AEMSLevelSequenceActor::ValidateStateInternal(
	FText& OutReason,
	const bool bRequirePlayer) const
{
	if (!CanAccessLevelSequence(OutReason, bRequirePlayer))
	{
		return false;
	}

	if (!PersistedState.bHasValidData)
	{
		OutReason = LOCTEXT(
			"LevelSequenceEmptyState",
			"No valid Level Sequence playback state has been captured or loaded.");
		return false;
	}

	if (PersistedState.Version
		!= FEMSLevelSequencePlaybackState::CurrentVersion)
	{
		OutReason = FText::Format(
			LOCTEXT(
				"LevelSequenceUnsupportedVersion",
				"Unsupported Level Sequence state version {0}."),
			FText::AsNumber(PersistedState.Version));
		return false;
	}

	if (PersistedState.SequenceAsset.IsNull()
		|| PersistedState.SequenceAsset != FSoftObjectPath(GetSequence()))
	{
		OutReason = LOCTEXT(
			"LevelSequenceAssetMismatch",
			"The saved and assigned Level Sequence assets differ.");
		return false;
	}

	if (!PersistedState.HasValidTimeScalars()
		|| !FMath::IsFinite(PersistedState.PlayRate)
		|| PersistedState.CompletedLoops < 0)
	{
		OutReason = LOCTEXT(
			"LevelSequenceInvalidState",
			"The saved Level Sequence state contains an invalid time, rate, or loop count.");
		return false;
	}

	OutReason = FText::GetEmpty();
	return true;
}

bool AEMSLevelSequenceActor::ValidateLevelSequenceState(
	FText& OutReason) const
{
	return ValidateStateInternal(OutReason, true);
}

bool AEMSLevelSequenceActor::AreRootPossessableBindingsReady() const
{
	ULevelSequencePlayer* Player = GetPersistenceSequencePlayer();
	ULevelSequence* Sequence = GetSequence();
	UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!Player || !MovieScene)
	{
		return false;
	}

	for (int32 Index = 0; Index < MovieScene->GetPossessableCount(); ++Index)
	{
		const FMovieScenePossessable& Possessable =
			MovieScene->GetPossessable(Index);
		const FMovieSceneObjectBindingID BindingId(
			UE::MovieScene::FFixedObjectBindingID(
				Possessable.GetGuid(),
				MovieSceneSequenceID::Root));

		if (Player->GetBoundObjects(BindingId).IsEmpty())
		{
			return false;
		}
	}

	return true;
}

bool AEMSLevelSequenceActor::IsLevelSequenceRuntimeReadyForRestore(
	bool& bOutWaitingForBindings) const
{
	bOutWaitingForBindings = false;

	const ULevelSequencePlayer* Player = GetPersistenceSequencePlayer();
	if (!Player || !Player->IsValid() || Player->GetSequence() != GetSequence())
	{
		return false;
	}

	if (!AreRootPossessableBindingsReady())
	{
		bOutWaitingForBindings = true;
		return false;
	}

	return true;
}

bool AEMSLevelSequenceActor::IsRelevantEMSLoadActive() const
{
	const ULevel* OwningLevel = GetLevel();
	if (OwningLevel
		&& FAsyncSaveHelpers::IsStreamAutoLoadActive(OwningLevel))
	{
		return true;
	}

	if (FAsyncSaveHelpers::IsAsyncLoadTaskActive(
		ESaveGameMode::MODE_Level,
		false))
	{
		return true;
	}

	const UEMSObject* EMS = BoundEMSObject
		? BoundEMSObject.Get()
		: UEMSObject::Get(this);
	return EMS
		&& EMS->AutoSaveLoadWorldPartition()
		&& !UEMSObject::SkipInitialWorldPartitionLoad()
		&& !EMS->WorldPartitionLoadComplete();
}

bool AEMSLevelSequenceActor::ApplyPersistedState(FText& OutReason)
{
	if (!ValidateStateInternal(OutReason, true))
	{
		return false;
	}

	ULevelSequencePlayer* Player = GetPersistenceSequencePlayer();
	if (!Player)
	{
		OutReason = LOCTEXT(
			"LevelSequencePlayerLost",
			"The Level Sequence player became unavailable during restoration.");
		return false;
	}

	const FQualifiedFrameTime CurrentTime = Player->GetCurrentTime();
	const FQualifiedFrameTime StartTime = Player->GetStartTime();
	const FQualifiedFrameTime EndTime = Player->GetEndTime();
	if (!CurrentTime.Rate.IsValid()
		|| !StartTime.Rate.IsValid()
		|| !EndTime.Rate.IsValid())
	{
		OutReason = LOCTEXT(
			"LevelSequenceInvalidPlaybackRange",
			"The Level Sequence player has an invalid playback range.");
		return false;
	}

	FFrameTime TargetTime = FFrameRate::TransformTime(
		PersistedState.GetPosition(),
		PersistedState.GetTickResolution(),
		CurrentTime.Rate);

	const FFrameTime RangeStart = FFrameRate::TransformTime(
		StartTime.Time,
		StartTime.Rate,
		CurrentTime.Rate);
	const FFrameTime RangeEnd = FFrameRate::TransformTime(
		EndTime.Time,
		EndTime.Rate,
		CurrentTime.Rate);

	const double TargetDecimal = TargetTime.AsDecimal();
	const double StartDecimal = RangeStart.AsDecimal();
	const double EndDecimal = RangeEnd.AsDecimal();
	if (!FMath::IsFinite(TargetDecimal)
		|| !FMath::IsFinite(StartDecimal)
		|| !FMath::IsFinite(EndDecimal)
		|| StartDecimal > EndDecimal)
	{
		OutReason = LOCTEXT(
			"LevelSequenceInvalidConvertedTime",
			"The saved Level Sequence time could not be converted into the current playback range.");
		return false;
	}

	if (TargetDecimal < StartDecimal || TargetDecimal > EndDecimal)
	{
		TargetTime = FFrameTime::FromDecimal(
			FMath::Clamp(TargetDecimal, StartDecimal, EndDecimal));

		UE_LOG(
			LogEMSAddonsLevelSequence,
			Warning,
			TEXT("Saved Level Sequence position was clamped to the current range. Actor=%s Asset=%s Saved=%s Target=%s Range=[%s,%s]"),
			*GetPathName(),
			*PersistedState.SequenceAsset.ToString(),
			*LexToShortString(PersistedState.GetPosition()),
			*LexToShortString(TargetTime),
			*LexToShortString(RangeStart),
			*LexToShortString(RangeEnd));
	}

	bApplyingRestore = true;
	Player->StopAtCurrentTime();

	FMovieSceneSequencePlaybackSettings RestoredSettings = PlaybackSettings;
	RestoredSettings.bAutoPlay = false;
	RestoredSettings.PlayRate = FMath::Abs(PersistedState.PlayRate);

	if (RestoredSettings.LoopCount.Value >= 0)
	{
		RestoredSettings.LoopCount.Value = FMath::Max(
			0,
			RestoredSettings.LoopCount.Value
				- PersistedState.CompletedLoops);
	}

	Player->SetPlaybackSettings(RestoredSettings);
	Player->SetPlayRate(RestoredSettings.PlayRate);

	const EUpdatePositionMethod UpdateMethod =
		PositionRestorePolicy
			== EEMSLevelSequencePositionRestorePolicy::JumpAndEvaluate
		? EUpdatePositionMethod::Jump
		: EUpdatePositionMethod::Play;

	FMovieSceneSequencePlaybackParams PlaybackParams(
		TargetTime,
		UpdateMethod);
	PlaybackParams.bHasJumped =
		UpdateMethod == EUpdatePositionMethod::Jump;
	Player->SetPlaybackPosition(PlaybackParams);

	if (PersistedState.PlaybackStatus == EEMSLevelSequencePlaybackStatus::Finished)
	{
		Player->StopAtCurrentTime();
		bRuntimeFinished = true;
	}
	else if (
		PersistedState.PlaybackStatus
			== EEMSLevelSequencePlaybackStatus::Playing
		&& bResumeIfPlaying)
	{
		if (PersistedState.bReversePlayback)
		{
			Player->PlayReverse();
		}
		else
		{
			Player->PlayLooping(RestoredSettings.LoopCount.Value);

			if (PersistedState.bReversePlayback != Player->IsReversed())
			{
				Player->ChangePlaybackDirection();
			}
		}

		bRuntimeFinished = false;
	}
	else if (
		PersistedState.PlaybackStatus
			== EEMSLevelSequencePlaybackStatus::Paused
		|| PersistedState.PlaybackStatus
			== EEMSLevelSequencePlaybackStatus::Playing)
	{
		if (PersistedState.bReversePlayback)
		{
			Player->PlayReverse();
		}
		else
		{
			Player->Play();
		}
		Player->Pause();
		bRuntimeFinished = false;
	}
	else
	{
		Player->StopAtCurrentTime();
		bRuntimeFinished = false;
	}

	bRuntimeHasStarted = PersistedState.bHasStarted;
	RuntimeCompletedLoops = PersistedState.CompletedLoops;
	bApplyingRestore = false;
	OutReason = FText::GetEmpty();
	return true;
}

bool AEMSLevelSequenceActor::RestoreLevelSequenceState()
{
	FText Reason;
	if (!ValidateStateInternal(Reason, false))
	{
		FailRestore(Reason);
		return false;
	}

	CancelPendingRestore(false);
	SuppressAutoPlay();
	bRestoreRequested = true;
	bLevelLoadCompletionReceived = true;
	RestoreStartSeconds = GetDeferralClockSeconds();
	bRestoreClockStarted = true;
	RestorePhase = EEMSAddonRestorePhase::Pending;
	LastRestoreResult = FEMSAddonResult(
		EEMSAddonResultCode::Pending,
		LOCTEXT(
			"LevelSequenceRestorePending",
			"Level Sequence restoration is pending."));
	OnLevelSequenceRestoreStarted.Broadcast();
	AttemptRestore();

	return RestorePhase != EEMSAddonRestorePhase::Failed
		&& RestorePhase != EEMSAddonRestorePhase::Canceled;
}

void AEMSLevelSequenceActor::QueueRestoreForEMSCompletion()
{
	FText Reason;
	if (!ValidateStateInternal(Reason, false))
	{
		FailRestore(Reason);
		return;
	}

	CancelPendingRestore(false);
	SuppressAutoPlay();
	bRestoreRequested = true;
	bLevelLoadCompletionReceived = false;
	RestoreStartSeconds = GetDeferralClockSeconds();
	bRestoreClockStarted = true;
	RestorePhase = EEMSAddonRestorePhase::Pending;
	LastRestoreResult = FEMSAddonResult(
		EEMSAddonResultCode::Pending,
		LOCTEXT(
			"LevelSequenceWaitingForEMS",
			"Waiting for EMS to finish restoring the owning level."));
	OnLevelSequenceRestoreStarted.Broadcast();

	BindEMSCompletionDelegates();
	if (!BoundEMSObject)
	{
		StartDeferredRestore();
		return;
	}

	ScheduleRestoreRetry();
}

void AEMSLevelSequenceActor::StartDeferredRestore()
{
	UnbindEMSCompletionDelegates();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RestoreRetryTimer);
	}
	bLevelLoadCompletionReceived = true;
	RestorePhase = EEMSAddonRestorePhase::Pending;
	AttemptRestore();
}

void AEMSLevelSequenceActor::AttemptRestore()
{
	if (bIsEndingPlay)
	{
		CancelPendingRestore(true);
		return;
	}

	if (!bLevelLoadCompletionReceived)
	{
		ScheduleRestoreRetry();
		return;
	}

	FText Reason;
	if (!ValidateStateInternal(Reason, false))
	{
		FailRestore(Reason);
		return;
	}

	if (!GetPersistenceSequencePlayer())
	{
		InitializePlayer();
		BindSequencePlayerDelegates();
	}

	bool bWaitingForBindings = false;
	if (!IsLevelSequenceRuntimeReadyForRestore(bWaitingForBindings))
	{
		ScheduleRestoreRetry();
		return;
	}

	if (!ApplyPersistedState(Reason))
	{
		FailRestore(Reason);
		return;
	}

	FinalizeRestore();
}

void AEMSLevelSequenceActor::ScheduleRestoreRetry()
{
	const double TimeoutSeconds = FMath::Max(0.05f, RestoreTimeout);
	if (!bRestoreClockStarted)
	{
		RestoreStartSeconds = GetDeferralClockSeconds();
		bRestoreClockStarted = true;
	}

	if (GetDeferralClockSeconds() - RestoreStartSeconds >= TimeoutSeconds)
	{
		FailRestore(LOCTEXT(
			"LevelSequenceRestoreTimeout",
			"Level Sequence restoration timed out while waiting for EMS, the player, or required bindings."));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		FailRestore(LOCTEXT(
			"LevelSequenceRestoreNoWorld",
			"Level Sequence restoration cannot retry without a valid world."));
		return;
	}

	RestorePhase = EEMSAddonRestorePhase::Pending;
	World->GetTimerManager().ClearTimer(RestoreRetryTimer);
	RestoreRetryTimer = World->GetTimerManager().SetTimerForNextTick(
		this,
		&AEMSLevelSequenceActor::AttemptRestore);
}

void AEMSLevelSequenceActor::FinalizeRestore()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RestoreRetryTimer);
	}

	UnbindEMSCompletionDelegates();
	bInitialAutoPlayDecisionPending = false;
	bInitialAutoPlayDecisionFinal = true;
	ReleaseAutoPlaySuppression();
	bRestoreRequested = false;
	RestorePhase = EEMSAddonRestorePhase::Complete;
	LastRestoreResult = FEMSAddonResult::Success();
	OnLevelSequenceRestoreFinished.Broadcast(LastRestoreResult);
}

void AEMSLevelSequenceActor::FailRestore(const FText& Reason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RestoreRetryTimer);
	}

	UnbindEMSCompletionDelegates();
	bRestoreRequested = false;
	bApplyingRestore = false;
	RestorePhase = EEMSAddonRestorePhase::Failed;
	LastRestoreResult = FEMSAddonResult(
		EEMSAddonResultCode::RestoreFailed,
		Reason);
	LogRestoreIssue(Reason);
	OnLevelSequenceRestoreFinished.Broadcast(LastRestoreResult);

	if (!bIsEndingPlay)
	{
		bInitialAutoPlayDecisionFinal = false;
		bInitialAutoPlayDecisionPending = false;
		ScheduleInitialAutoPlay();
	}
}

void AEMSLevelSequenceActor::CancelPendingRestore(
	const bool bReportCanceled)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RestoreRetryTimer);
	}

	UnbindEMSCompletionDelegates();
	bRestoreClockStarted = false;

	if (bReportCanceled && IsLevelSequenceRestorePending())
	{
		RestorePhase = EEMSAddonRestorePhase::Canceled;
		LastRestoreResult = FEMSAddonResult(
			EEMSAddonResultCode::Canceled,
			LOCTEXT(
				"LevelSequenceRestoreCanceled",
				"Level Sequence restoration was canceled during teardown."));
		OnLevelSequenceRestoreFinished.Broadcast(LastRestoreResult);
	}
}

bool AEMSLevelSequenceActor::IsLevelSequenceRestorePending() const
{
	return RestorePhase == EEMSAddonRestorePhase::Pending;
}

EEMSAddonRestorePhase
AEMSLevelSequenceActor::GetLevelSequenceRestorePhase() const
{
	return RestorePhase;
}

FEMSAddonResult AEMSLevelSequenceActor::GetLevelSequenceRestoreResult() const
{
	return LastRestoreResult;
}

/**
 * A client has nothing to do here and is not at fault for it.
 *
 * The authority rule is also enforced inside capture and restore, but reaching
 * it through those reports a failed restore and logs a warning for what is the
 * designed outcome on every client, every load. Leaving early also leaves a
 * playing sequence alone, which the restore path would otherwise stop.
 */
void AEMSLevelSequenceActor::SkipPersistenceWithoutAuthority()
{
	RestorePhase = EEMSAddonRestorePhase::Idle;
	LastRestoreResult = FEMSAddonResult(
		EEMSAddonResultCode::Skipped,
		LOCTEXT(
			"LevelSequenceSkippedWithoutAuthority",
			"Level Sequence playback state is server-authoritative and is neither captured nor restored here."));
}

void AEMSLevelSequenceActor::ActorPreSave_Implementation()
{
	EMSAddons::RunOnGameThread(
		[this]()
		{
			// The net mode is read on the game thread with everything else.
			if (EMSAddons::HasPersistenceAuthority(
				this,
				GetLevelSequenceNetMode()))
			{
				CaptureLevelSequenceState();
			}
		});
}

void AEMSLevelSequenceActor::ActorPreLoad_Implementation()
{
	if (!EMSAddons::HasPersistenceAuthority(this, GetLevelSequenceNetMode()))
	{
		SkipPersistenceWithoutAuthority();
		return;
	}

	CancelPendingRestore(false);
	SuppressAutoPlay();
	bInitialAutoPlayDecisionPending = false;
	bInitialAutoPlayDecisionFinal = false;
	bRestoreRequested = true;
	bLevelLoadCompletionReceived = false;

	if (ULevelSequencePlayer* Player = GetPersistenceSequencePlayer())
	{
		Player->StopAtCurrentTime();
	}

	RestorePhase = EEMSAddonRestorePhase::Idle;
	LastRestoreResult = FEMSAddonResult(
		EEMSAddonResultCode::Pending,
		LOCTEXT(
			"LevelSequenceWaitingForData",
			"Waiting for Level Sequence save data."));
}

void AEMSLevelSequenceActor::ActorLoaded_Implementation()
{
	if (!EMSAddons::HasPersistenceAuthority(this, GetLevelSequenceNetMode()))
	{
		SkipPersistenceWithoutAuthority();
		return;
	}

	QueueRestoreForEMSCompletion();
}

bool AEMSLevelSequenceActor::IsEMSAddonRestoreComplete() const
{
	return !IsLevelSequenceRestorePending();
}

FEMSAddonResult
AEMSLevelSequenceActor::GetEMSAddonRestoreResult() const
{
	return LastRestoreResult;
}

ULevelSequencePlayer*
AEMSLevelSequenceActor::GetPersistenceSequencePlayer() const
{
	return GetSequencePlayer();
}

void AEMSLevelSequenceActor::BindEMSCompletionDelegates()
{
	UnbindEMSCompletionDelegates();
	BoundEMSObject = UEMSObject::Get(this);
	if (!BoundEMSObject)
	{
		return;
	}

	BoundEMSObject->OnLevelLoaded.AddUniqueDynamic(
		this,
		&AEMSLevelSequenceActor::HandleEMSLevelLoaded);
	BoundEMSObject->OnPartitionLoaded.AddUniqueDynamic(
		this,
		&AEMSLevelSequenceActor::HandleEMSLevelLoaded);
}

void AEMSLevelSequenceActor::UnbindEMSCompletionDelegates()
{
	if (!BoundEMSObject)
	{
		return;
	}

	BoundEMSObject->OnLevelLoaded.RemoveDynamic(
		this,
		&AEMSLevelSequenceActor::HandleEMSLevelLoaded);
	BoundEMSObject->OnPartitionLoaded.RemoveDynamic(
		this,
		&AEMSLevelSequenceActor::HandleEMSLevelLoaded);
	BoundEMSObject = nullptr;
}

void AEMSLevelSequenceActor::HandleEMSLevelLoaded(
	const TArray<TSoftObjectPtr<AActor>>& LoadedActors)
{
	const bool bListContainsThisActor = LoadedActors.ContainsByPredicate(
		[this](const TSoftObjectPtr<AActor>& LoadedActor)
		{
			return LoadedActor.Get() == this;
		});

	if (bRestoreRequested && bListContainsThisActor)
	{
		StartDeferredRestore();
		return;
	}

	if (!bRestoreRequested
		&& bInitialAutoPlayDecisionPending
		&& !IsRelevantEMSLoadActive())
	{
		HandleInitialAutoPlay();
	}
}

void AEMSLevelSequenceActor::BindSequencePlayerDelegates()
{
	ULevelSequencePlayer* Player = GetPersistenceSequencePlayer();
	if (BoundSequencePlayer == Player)
	{
		return;
	}

	UnbindSequencePlayerDelegates();
	BoundSequencePlayer = Player;
	if (!Player)
	{
		return;
	}

	Player->OnPlay.AddUniqueDynamic(
		this,
		&AEMSLevelSequenceActor::HandleSequencePlayed);
	Player->OnPlayReverse.AddUniqueDynamic(
		this,
		&AEMSLevelSequenceActor::HandleSequencePlayedReverse);
	Player->OnPause.AddUniqueDynamic(
		this,
		&AEMSLevelSequenceActor::HandleSequencePaused);
	Player->OnStop.AddUniqueDynamic(
		this,
		&AEMSLevelSequenceActor::HandleSequenceStopped);
	Player->OnFinished.AddUniqueDynamic(
		this,
		&AEMSLevelSequenceActor::HandleSequenceFinished);
	Player->OnSequenceUpdated().AddUObject(
		this,
		&AEMSLevelSequenceActor::HandleSequenceUpdated);
}

void AEMSLevelSequenceActor::UnbindSequencePlayerDelegates()
{
	if (!BoundSequencePlayer)
	{
		return;
	}

	BoundSequencePlayer->OnPlay.RemoveDynamic(
		this,
		&AEMSLevelSequenceActor::HandleSequencePlayed);
	BoundSequencePlayer->OnPlayReverse.RemoveDynamic(
		this,
		&AEMSLevelSequenceActor::HandleSequencePlayedReverse);
	BoundSequencePlayer->OnPause.RemoveDynamic(
		this,
		&AEMSLevelSequenceActor::HandleSequencePaused);
	BoundSequencePlayer->OnStop.RemoveDynamic(
		this,
		&AEMSLevelSequenceActor::HandleSequenceStopped);
	BoundSequencePlayer->OnFinished.RemoveDynamic(
		this,
		&AEMSLevelSequenceActor::HandleSequenceFinished);
	BoundSequencePlayer->OnSequenceUpdated().RemoveAll(this);
	BoundSequencePlayer = nullptr;
}

void AEMSLevelSequenceActor::SuppressAutoPlay()
{
	if (!bAutoPlaySuppressed)
	{
		bAuthoredAutoPlay = PlaybackSettings.bAutoPlay;
	}

	PlaybackSettings.bAutoPlay = false;
	bAutoPlaySuppressed = true;
	bInitialAutoPlayDecisionPending = false;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(InitialAutoPlayTimer);
	}
}

void AEMSLevelSequenceActor::ReleaseAutoPlaySuppression()
{
	if (!bAutoPlaySuppressed)
	{
		return;
	}

	PlaybackSettings.bAutoPlay = bAuthoredAutoPlay;
	bAutoPlaySuppressed = false;
}

void AEMSLevelSequenceActor::ScheduleInitialAutoPlay()
{
	if (bRestoreRequested || bIsEndingPlay)
	{
		return;
	}

	bInitialAutoPlayDecisionPending = true;
	InitialAutoPlayWaitStartSeconds = GetDeferralClockSeconds();
	BindEMSCompletionDelegates();
	if (UWorld* World = GetWorld())
	{
		InitialAutoPlayTimer = World->GetTimerManager().SetTimerForNextTick(
			this,
			&AEMSLevelSequenceActor::HandleInitialAutoPlay);
	}
}

void AEMSLevelSequenceActor::HandleInitialAutoPlay()
{
	if (bRestoreRequested || bIsEndingPlay)
	{
		return;
	}

	if (!bInitialAutoPlayDecisionFinal)
	{
		const double WaitedSeconds =
			GetDeferralClockSeconds() - InitialAutoPlayWaitStartSeconds;
		const bool bWaitExhausted =
			WaitedSeconds >= MaxInitialAutoPlayWaitSeconds;

		if (IsRelevantEMSLoadActive() && !bWaitExhausted)
		{
			if (UWorld* World = GetWorld())
			{
				InitialAutoPlayTimer =
					World->GetTimerManager().SetTimerForNextTick(
						this,
						&AEMSLevelSequenceActor::HandleInitialAutoPlay);
			}
			return;
		}

		if (bWaitExhausted)
		{
			UE_LOG(
				LogEMSAddonsLevelSequence,
				Warning,
				TEXT("EMS never reported its load as finished, so authored autoplay proceeds ungated. A restore arriving later still takes over. Actor=%s Waited=%.1fs"),
				*GetPathName(),
				WaitedSeconds);
		}

		bInitialAutoPlayDecisionPending = false;
		bInitialAutoPlayDecisionFinal = true;
		InitialAutoPlayStartSeconds = GetDeferralClockSeconds();
		UnbindEMSCompletionDelegates();
	}

	StartAuthoredAutoPlayWhenReady();
}

void AEMSLevelSequenceActor::StartAuthoredAutoPlayWhenReady()
{
	if (bRestoreRequested || bIsEndingPlay)
	{
		return;
	}

	ULevelSequencePlayer* Player = GetPersistenceSequencePlayer();
	if (!Player)
	{
		InitializePlayer();
		BindSequencePlayerDelegates();
		Player = GetPersistenceSequencePlayer();
	}

	if (!Player || !Player->IsValid())
	{
		const double TimeoutSeconds = FMath::Max(0.05f, RestoreTimeout);
		if (GetDeferralClockSeconds() - InitialAutoPlayStartSeconds
			< TimeoutSeconds)
		{
			if (UWorld* World = GetWorld())
			{
				InitialAutoPlayTimer =
					World->GetTimerManager().SetTimerForNextTick(
						this,
						&AEMSLevelSequenceActor::HandleInitialAutoPlay);
			}
		}
		else
		{
			ReleaseAutoPlaySuppression();
		}
		return;
	}

	ReleaseAutoPlaySuppression();
	if (bAuthoredAutoPlay)
	{
		Player->Play();
	}
}

void AEMSLevelSequenceActor::HandleSequencePlayed()
{
	if (!bApplyingRestore)
	{
		bRuntimeHasStarted = true;
		bRuntimeFinished = false;
	}
}

void AEMSLevelSequenceActor::HandleSequencePlayedReverse()
{
	HandleSequencePlayed();
}

void AEMSLevelSequenceActor::HandleSequencePaused()
{
	if (!bApplyingRestore)
	{
		bRuntimeHasStarted = true;
	}
}

void AEMSLevelSequenceActor::HandleSequenceStopped()
{
	if (!bApplyingRestore)
	{
		bRuntimeFinished = false;
	}
}

void AEMSLevelSequenceActor::HandleSequenceFinished()
{
	if (!bApplyingRestore)
	{
		bRuntimeHasStarted = true;
		bRuntimeFinished = true;
	}
}

void AEMSLevelSequenceActor::HandleSequenceUpdated(
	const UMovieSceneSequencePlayer& Player,
	const FFrameTime CurrentTime,
	const FFrameTime PreviousTime)
{
	if (bApplyingRestore || !Player.IsPlaying())
	{
		return;
	}

	const bool bWrappedForward =
		!Player.IsReversed()
		&& CurrentTime.AsDecimal() < PreviousTime.AsDecimal();
	const bool bWrappedReverse =
		Player.IsReversed()
		&& CurrentTime.AsDecimal() > PreviousTime.AsDecimal();
	if (bWrappedForward || bWrappedReverse)
	{
		++RuntimeCompletedLoops;
	}
}

void AEMSLevelSequenceActor::LogRestoreIssue(const FText& Reason) const
{
	const ULevelSequencePlayer* Player = GetPersistenceSequencePlayer();
	const FQualifiedFrameTime CurrentTime = Player
		? Player->GetCurrentTime()
		: FQualifiedFrameTime();

	// The saved scalars are logged raw. This also runs for states that failed
	// validation, where they cannot be trusted to form a usable FFrameTime.
	UE_LOG(
		LogEMSAddonsLevelSequence,
		Warning,
		TEXT("EMS Level Sequence operation skipped or failed. Actor=%s Level=%s Asset=%s StateVersion=%d SavedPosition=%d.%f SavedRate=%d/%d CurrentPosition=%s CurrentRate=%d/%d Status=%d Phase=%d Reason=%s"),
		*GetPathName(),
		GetLevel() ? *GetLevel()->GetPathName() : TEXT("<none>"),
		GetSequence() ? *GetSequence()->GetPathName() : TEXT("<none>"),
		PersistedState.Version,
		PersistedState.PositionFrame,
		PersistedState.PositionSubFrame,
		PersistedState.TickResolutionNumerator,
		PersistedState.TickResolutionDenominator,
		*LexToShortString(CurrentTime.Time),
		CurrentTime.Rate.Numerator,
		CurrentTime.Rate.Denominator,
		static_cast<int32>(PersistedState.PlaybackStatus),
		static_cast<int32>(RestorePhase),
		*Reason.ToString());
}

#undef LOCTEXT_NAMESPACE
