//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSCheckpoint.h"

#include "Components/BillboardComponent.h"
#include "Components/BoxComponent.h"
#include "EMSAddonsAutosave.h"
#include "EMSAutosaveSubsystem.h"
#include "EMSCheckpointUtils.h"
#include "EMSMisc.h"
#include "EMSObject.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

#define LOCTEXT_NAMESPACE "EMSCheckpoint"

namespace
{
	constexpr float CheckpointMetadataWatchInterval = 0.25f;
}

AEMSCheckpoint::AEMSCheckpoint()
{
	PrimaryActorTick.bCanEverTick = false;
	Trigger = CreateDefaultSubobject<UBoxComponent>(TEXT("CheckpointTrigger"));
	Trigger->SetBoxExtent(FVector(100.0, 100.0, 100.0));
	Trigger->SetCollisionProfileName(TEXT("Trigger"));
	SetRootComponent(Trigger);

#if WITH_EDITORONLY_DATA
	CheckpointSprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("CheckpointSprite"));
	if (CheckpointSprite)
	{
		static ConstructorHelpers::FObjectFinder<UTexture2D> SpriteTexture(TEXT("/Engine/EditorResources/S_Player"));
		if (SpriteTexture.Succeeded())
		{
			CheckpointSprite->SetSprite(SpriteTexture.Object);
		}
		CheckpointSprite->SpriteInfo.Category = TEXT("EasyMultiSave");
		CheckpointSprite->SpriteInfo.DisplayName = LOCTEXT("SpriteCategory", "Easy Multi Save");
		CheckpointSprite->bIsScreenSizeScaled = true;
		CheckpointSprite->SetupAttachment(Trigger);
	}
#endif
}

void AEMSCheckpoint::BeginPlay()
{
	Super::BeginPlay();
	EnsureCheckpointId();

	// Always start unarmed. A load may restore the pawn before or during BeginPlay,
	// and an active trigger must never interpret restoration as gameplay entry.
	SavedTriggerCollision = Trigger->GetCollisionEnabled();
	Trigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	bWaitingForCheckpointLoad = true;

	if (UEMSObject* EMS = UEMSObject::Get(this))
	{
		EMS->OnPlayerLoaded.AddUniqueDynamic(this, &AEMSCheckpoint::HandleEMSPlayerLoaded);
	}

	UEMSAutosaveSubsystem* Autosave = nullptr;
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		Autosave = GameInstance->GetSubsystem<UEMSAutosaveSubsystem>();
		if (Autosave)
		{
			Autosave->OnCheckpointLoaded.AddUniqueDynamic(this, &AEMSCheckpoint::HandleCheckpointLoaded);
			Autosave->OnCheckpointLoadFailed.AddUniqueDynamic(this, &AEMSCheckpoint::HandleCheckpointLoadFailed);
		}
	}

	// A checkpoint whose level or World Partition cell streamed in after the load
	// finished missed the On Checkpoint Loaded broadcast, so it claims the player
	// placement here instead. Same handling as the broadcast path below it.
	if (Autosave && Autosave->ConsumeCheckpointPlayerPlacement(this))
	{
		bRestorePlayerToCenterOnArm = true;
		MarkActivationCommitted();
		RestoreTriggerCollision();
		return;
	}

	// No matching checkpoint data means this actor cannot be the stored checkpoint
	// destination and may arm immediately. Matching data stays locked until EMS
	// completes player restoration or that checkpoint data is no longer current.
	RefreshTriggerCollisionFromSaveData(Autosave);
}

void AEMSCheckpoint::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The delegates below live on the game instance and outlive this actor. Checkpoints
	// can stream in and out repeatedly, so release them instead of leaving one binding
	// behind per streamed-in instance.
	if (UEMSObject* EMS = UEMSObject::Get(this))
	{
		EMS->OnPlayerLoaded.RemoveDynamic(this, &AEMSCheckpoint::HandleEMSPlayerLoaded);
	}

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UEMSAutosaveSubsystem* Autosave = GameInstance->GetSubsystem<UEMSAutosaveSubsystem>())
		{
			Autosave->OnCheckpointLoaded.RemoveDynamic(this, &AEMSCheckpoint::HandleCheckpointLoaded);
			Autosave->OnCheckpointLoadFailed.RemoveDynamic(this, &AEMSCheckpoint::HandleCheckpointLoadFailed);
		}
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
	}

	// A hidden streamed level can later BeginPlay the same actor again. Leave the
	// trigger in its normal configured state so the next BeginPlay never mistakes a
	// temporary load lock for the checkpoint's authored collision setting.
	if (Trigger)
	{
		Trigger->OnComponentBeginOverlap.RemoveDynamic(this, &AEMSCheckpoint::HandleTriggerBeginOverlap);
		Trigger->SetCollisionEnabled(SavedTriggerCollision);
	}
	bWaitingForCheckpointLoad = false;
	bLoadWatchScheduled = false;
	bArmWatchScheduled = false;

	Super::EndPlay(EndPlayReason);
}

void AEMSCheckpoint::PostActorCreated()
{
	Super::PostActorCreated();
	EnsureCheckpointId();
}

void AEMSCheckpoint::PostLoad()
{
	Super::PostLoad();
	EnsureCheckpointId();
}

void AEMSCheckpoint::EnsureCheckpointId()
{
	if (!HasAnyFlags(RF_ClassDefaultObject) && !CheckpointId.IsValid())
	{
		CheckpointId = FGuid::NewGuid();
	}
}

bool AEMSCheckpoint::ActivateCheckpoint()
{
	if (bWaitingForCheckpointLoad || bActivationPending || (bTriggerOnce && bActivatedThisSession))
	{
		return false;
	}

	// A repeatable checkpoint represents a fresh capture on every accepted
	// activation. Force the save path so matching checkpoint identity cannot turn
	// a genuine leave/re-enter into an event-only no-op.
	if (!bTriggerOnce)
	{
		bForceSaveNextActivation = true;
	}

	EnsureCheckpointId();
	UGameInstance* GameInstance = GetGameInstance();
	UEMSAutosaveSubsystem* Autosave = GameInstance ? GameInstance->GetSubsystem<UEMSAutosaveSubsystem>() : nullptr;
	const TWeakObjectPtr<AEMSCheckpoint> WeakThis(this);
	if (!Autosave || !Autosave->ActivateCheckpoint(this) || !WeakThis.IsValid())
	{
		return false;
	}

	WeakThis->OnCheckpointActivated.Broadcast();
	return true;
}

void AEMSCheckpoint::ResetCheckpoint()
{
	bActivatedThisSession = false;
	bActivationPending = false;
	bForceSaveNextActivation = true;
	bRestorePlayerToCenterOnArm = false;

	UGameInstance* GameInstance = GetGameInstance();
	UEMSAutosaveSubsystem* Autosave = GameInstance ? GameInstance->GetSubsystem<UEMSAutosaveSubsystem>() : nullptr;
	const bool bAddonCheckpointLoad = Autosave && Autosave->IsCheckpointLoadInProgress();
	const bool bPlayerLoadActive = FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_Player, false);
	if (bAddonCheckpointLoad || bPlayerLoadActive)
	{
		DisableTriggerForCheckpointLoad();
		if (!bAddonCheckpointLoad)
		{
			ScheduleLoadWatch();
		}
		return;
	}

	// Reset is an explicit gameplay action and overrides an already-committed
	// checkpoint record. The next accepted activation must be allowed to happen.
	RestoreTriggerCollision();
}

void AEMSCheckpoint::MarkActivationPending()
{
	bActivationPending = true;
	if (bTriggerOnce)
	{
		bActivatedThisSession = true;
	}
}

void AEMSCheckpoint::MarkActivationCommitted()
{
	bActivationPending = false;
	bActivatedThisSession = true;
	bForceSaveNextActivation = false;
}

void AEMSCheckpoint::MarkActivationFailed()
{
	bActivationPending = false;
	if (bTriggerOnce)
	{
		bActivatedThisSession = false;
	}
}

bool AEMSCheckpoint::HasUniqueCheckpointId() const
{
	if (!CheckpointId.IsValid() || !GetWorld())
	{
		return false;
	}

	for (TActorIterator<AEMSCheckpoint> It(GetWorld()); It; ++It)
	{
		if (*It != this && It->CheckpointId == CheckpointId)
		{
			return false;
		}
	}
	return true;
}

bool AEMSCheckpoint::MatchesCheckpointRecord(const FEMSCheckpointRecord& Checkpoint) const
{
	if (!Checkpoint.IsValid() || Checkpoint.CheckpointId != CheckpointId)
	{
		return false;
	}

	return EMSCheckpoint::IsSameWorld(GetWorld(), Checkpoint.World);
}

void AEMSCheckpoint::DisableTriggerForCheckpointLoad()
{
	if (!Trigger)
	{
		return;
	}

	if (!bWaitingForCheckpointLoad)
	{
		SavedTriggerCollision = Trigger->GetCollisionEnabled();
	}

	// Unarm before collision is disabled. This ordering covers a load that starts
	// after normal gameplay has already armed the checkpoint.
	Trigger->OnComponentBeginOverlap.RemoveDynamic(this, &AEMSCheckpoint::HandleTriggerBeginOverlap);
	bWaitingForCheckpointLoad = true;
	Trigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AEMSCheckpoint::RestoreTriggerCollision()
{
	if (!Trigger)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CheckpointMetadataWatchTimer);
	}

	// Restore collision while BeginOverlap is deliberately unbound. Existing or
	// newly spawned pawns can settle their overlap state before the callback is armed.
	Trigger->OnComponentBeginOverlap.RemoveDynamic(this, &AEMSCheckpoint::HandleTriggerBeginOverlap);
	bWaitingForCheckpointLoad = false;
	Trigger->SetCollisionEnabled(SavedTriggerCollision);

	if ((SavedTriggerCollision != ECollisionEnabled::NoCollision || bRestorePlayerToCenterOnArm) && !bArmWatchScheduled)
	{
		if (UWorld* World = GetWorld())
		{
			bArmWatchScheduled = true;
			World->GetTimerManager().SetTimerForNextTick(this, &AEMSCheckpoint::ResolveArmWatch);
		}
	}
}

void AEMSCheckpoint::ArmTriggerWhenPlayerReady()
{
	if (!Trigger || bWaitingForCheckpointLoad)
	{
		return;
	}

	APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!PlayerPawn)
	{
		if (!bArmWatchScheduled)
		{
			if (UWorld* World = GetWorld())
			{
				bArmWatchScheduled = true;
				World->GetTimerManager().SetTimerForNextTick(this, &AEMSCheckpoint::ResolveArmWatch);
			}
		}
		return;
	}

	PlaceRestoredPlayer(PlayerPawn);

	if (SavedTriggerCollision != ECollisionEnabled::NoCollision)
	{
		// At least one tick has passed since collision was restored. The local player
		// exists and startup/restore overlap establishment happened while unbound.
		Trigger->OnComponentBeginOverlap.AddUniqueDynamic(this, &AEMSCheckpoint::HandleTriggerBeginOverlap);
	}
}

bool AEMSCheckpoint::PlaceRestoredPlayer(APawn* PlayerPawn)
{
	//One shot, and the check belongs here rather than in the caller: a checkpoint
	//streaming back in must not teleport the player long after its restore, and a
	//failed move must not be retried on a later arm either.
	if (!bRestorePlayerToCenterOnArm)
	{
		return false;
	}
	bRestorePlayerToCenterOnArm = false;
	if (!PlayerPawn || !Trigger)
	{
		return false;
	}

	//The trigger's component location is the box center, which is the authored
	//checkpoint position rather than wherever the actor's root happens to sit.
	if (!PlayerPawn->SetActorLocation(
		Trigger->GetComponentLocation(),
		false,
		nullptr,
		ETeleportType::TeleportPhysics))
	{
		UE_LOG(
			LogEMSAddonsAutosave,
			Warning,
			TEXT("Checkpoint restore could not move player pawn %s to checkpoint center %s."),
			*PlayerPawn->GetPathName(),
			*GetPathName());
		return false;
	}
	return true;
}

void AEMSCheckpoint::ResolveArmWatch()
{
	bArmWatchScheduled = false;
	ArmTriggerWhenPlayerReady();
}

void AEMSCheckpoint::RefreshTriggerCollisionFromSaveData(UEMSAutosaveSubsystem* Autosave)
{
	const bool bAddonCheckpointLoad = Autosave && Autosave->IsCheckpointLoadInProgress();
	const bool bPlayerLoadActive = FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_Player, false);
	if (bAddonCheckpointLoad || bPlayerLoadActive)
	{
		DisableTriggerForCheckpointLoad();
		if (!bAddonCheckpointLoad)
		{
			ScheduleLoadWatch();
		}
		return;
	}

	const bool bMatchesCurrentCheckpoint = Autosave && MatchesCheckpointRecord(Autosave->GetCurrentCheckpoint());
	if (!bMatchesCurrentCheckpoint || (!bTriggerOnce && bActivatedThisSession))
	{
		// Nothing is currently restoring into this actor. A repeatable checkpoint
		// that already activated this session must also rearm after streaming back in.
		RestoreTriggerCollision();
		return;
	}

	// Only the checkpoint represented by the current slot reaches this path, so a
	// low-frequency metadata watch costs a single timer and handles slot/user clears
	// even though EMS does not expose a dedicated current-slot-changed delegate.
	DisableTriggerForCheckpointLoad();
	ScheduleCheckpointMetadataWatch();
}

void AEMSCheckpoint::ScheduleLoadWatch()
{
	if (!bWaitingForCheckpointLoad || bLoadWatchScheduled)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		bLoadWatchScheduled = true;
		World->GetTimerManager().SetTimerForNextTick(this, &AEMSCheckpoint::ResolveLoadWatch);
	}
}

void AEMSCheckpoint::ResolveLoadWatch()
{
	bLoadWatchScheduled = false;
	if (!bWaitingForCheckpointLoad)
	{
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UEMSAutosaveSubsystem* Autosave = GameInstance ? GameInstance->GetSubsystem<UEMSAutosaveSubsystem>() : nullptr;
	if (Autosave && Autosave->IsCheckpointLoadInProgress())
	{
		// The explicit checkpoint path owns its success/failure state and will arm
		// through OnCheckpointLoaded. Do not infer completion from the EMS task here.
		return;
	}

	if (FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_Player, false))
	{
		ScheduleLoadWatch();
		return;
	}

	RestoreTriggerCollision();
}

void AEMSCheckpoint::ScheduleCheckpointMetadataWatch()
{
	if (!bWaitingForCheckpointLoad)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (!World->GetTimerManager().IsTimerActive(CheckpointMetadataWatchTimer))
		{
			World->GetTimerManager().SetTimer(
				CheckpointMetadataWatchTimer,
				this,
				&AEMSCheckpoint::ResolveCheckpointMetadataWatch,
				CheckpointMetadataWatchInterval,
				false);
		}
	}
}

void AEMSCheckpoint::ResolveCheckpointMetadataWatch()
{
	if (!bWaitingForCheckpointLoad)
	{
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UEMSAutosaveSubsystem* Autosave = GameInstance ? GameInstance->GetSubsystem<UEMSAutosaveSubsystem>() : nullptr;
	const bool bAddonCheckpointLoad = Autosave && Autosave->IsCheckpointLoadInProgress();
	const bool bPlayerLoadActive = FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_Player, false);
	if (bAddonCheckpointLoad || bPlayerLoadActive)
	{
		ScheduleCheckpointMetadataWatch();
		return;
	}

	const bool bMatchesCurrentCheckpoint = Autosave && MatchesCheckpointRecord(Autosave->GetCurrentCheckpoint());
	if (!bMatchesCurrentCheckpoint || (!bTriggerOnce && bActivatedThisSession))
	{
		RestoreTriggerCollision();
		return;
	}

	ScheduleCheckpointMetadataWatch();
}

#if WITH_EDITOR
void AEMSCheckpoint::RegenerateCheckpointId()
{
	Modify();
	CheckpointId = FGuid::NewGuid();
}

void AEMSCheckpoint::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	EnsureCheckpointId();
	if (!HasUniqueCheckpointId())
	{
		UE_LOG(LogEMSAddonsAutosave, Error, TEXT("Duplicate or invalid EMS Checkpoint ID. Actor=%s Id=%s"), *GetPathName(), *CheckpointId.ToString());
	}
}

void AEMSCheckpoint::PostDuplicate(EDuplicateMode::Type DuplicateMode)
{
	Super::PostDuplicate(DuplicateMode);
	if (DuplicateMode != EDuplicateMode::PIE)
	{
		RegenerateCheckpointId();
	}
}
#endif

void AEMSCheckpoint::HandleEMSPlayerLoaded(const APlayerController* LoadedPlayer)
{
	if (!LoadedPlayer)
	{
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UEMSAutosaveSubsystem* Autosave = GameInstance ? GameInstance->GetSubsystem<UEMSAutosaveSubsystem>() : nullptr;

	// Every player restore can move or reconstruct the pawn into any checkpoint,
	// not only the checkpoint whose metadata initiated Load Last Checkpoint. Unarm
	// this actor before any deferred overlap update can become gameplay activation.
	DisableTriggerForCheckpointLoad();

	if (Autosave && Autosave->IsCheckpointLoadInProgress())
	{
		// The explicit checkpoint path owns final success/failure. All checkpoint
		// actors stay locked until its completion event rearms them from metadata.
		return;
	}

	// For a combined player+level task, EMS may still be active after the player
	// phase. Otherwise RestoreTriggerCollision still defers BeginOverlap arming one
	// additional tick, consuming any load-generated overlap state.
	if (FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_Player, false))
	{
		ScheduleLoadWatch();
		return;
	}

	RestoreTriggerCollision();
}

void AEMSCheckpoint::HandleCheckpointLoaded(FEMSCheckpointRecord Checkpoint)
{
	if (!Checkpoint.IsValid())
	{
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UEMSAutosaveSubsystem* Autosave = GameInstance ? GameInstance->GetSubsystem<UEMSAutosaveSubsystem>() : nullptr;
	if (MatchesCheckpointRecord(Checkpoint))
	{
		// The checkpoint actor is the restore point. Keep overlap unarmed, move the
		// player to the trigger center on the delayed arm step, then allow future entry.
		// Claiming the handoff here stops a later stream-in from repeating the move.
		if (Autosave)
		{
			Autosave->ConsumeCheckpointPlayerPlacement(this);
		}
		bRestorePlayerToCenterOnArm = true;
		DisableTriggerForCheckpointLoad();
		MarkActivationCommitted();
		RestoreTriggerCollision();
		return;
	}

	// Other checkpoints were also unarmed for player restoration. Rearm them from
	// the current slot metadata after the complete checkpoint load has finished.
	RefreshTriggerCollisionFromSaveData(Autosave);
}

void AEMSCheckpoint::HandleCheckpointLoadFailed(FEMSCheckpointRecord Checkpoint)
{
	if (!Checkpoint.IsValid())
	{
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UEMSAutosaveSubsystem* Autosave = GameInstance ? GameInstance->GetSubsystem<UEMSAutosaveSubsystem>() : nullptr;

	// Failure does not consume the checkpoint record. The matching actor remains
	// locked by its metadata; every other actor is safely rearmed.
	RefreshTriggerCollisionFromSaveData(Autosave);
}

void AEMSCheckpoint::HandleTriggerBeginOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	if (!bActivateOnPlayerOverlap || !OtherActor || OtherActor != UGameplayStatics::GetPlayerPawn(this, 0))
	{
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UEMSAutosaveSubsystem* Autosave = GameInstance ? GameInstance->GetSubsystem<UEMSAutosaveSubsystem>() : nullptr;
	const bool bAddonCheckpointLoad = Autosave && Autosave->IsCheckpointLoadInProgress();
	const bool bPlayerLoadActive = FAsyncSaveHelpers::IsAsyncLoadTaskActive(ESaveGameMode::MODE_Player, false);
	if (bAddonCheckpointLoad || bPlayerLoadActive)
	{
		// A load-generated overlap is restoration, not gameplay entering a checkpoint.
		DisableTriggerForCheckpointLoad();
		if (!bAddonCheckpointLoad)
		{
			ScheduleLoadWatch();
		}
		return;
	}

	ActivateCheckpoint();
}

#undef LOCTEXT_NAMESPACE