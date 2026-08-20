//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSWorldPartitionRuntimeSubsystem.h"

#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "EMSActorSaveInterface.h"
#include "EMSAddonsActorBinary.h"
#include "EMSAddonsWorldPartition.h"
#include "EMSData.h"
#include "EMSObject.h"
#include "EMSTypes.h"
#include "EMSWorldPartitionCellResolver.h"
#include "EMSWorldPartitionRuntimeManager.h"
#include "EMSWorldPartitionRuntimeTypes.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Streaming/LevelStreamingDelegates.h"
#include "TimerManager.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionLevelStreamingDynamic.h"
#include "WorldPartition/WorldPartitionRuntimeLevelStreamingCell.h"

namespace
{
	bool IsAuthorityNetMode(const ENetMode NetMode)
	{
		return NetMode == NM_Standalone || NetMode == NM_ListenServer || NetMode == NM_DedicatedServer;
	}

	AController* GetNonPlayerController(AActor* Actor)
	{
		const APawn* Pawn = Cast<APawn>(Actor);
		AController* Controller = Pawn ? Pawn->GetController() : nullptr;
		return IsValid(Controller) && !Controller->IsPlayerController() ? Controller : nullptr;
	}

	void DestroyOrphanedController(AController* Controller)
	{
		if (IsValid(Controller) && !Controller->GetPawn())
		{
			Controller->Destroy();
		}
	}

	bool IsCellVisible(UWorld* World, const FGuid& CellGuid)
	{
		if (!IsValid(World) || !CellGuid.IsValid())
		{
			return !CellGuid.IsValid();
		}
		for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
		{
			const UWorldPartitionLevelStreamingDynamic* WorldPartitionStreaming =
				Cast<UWorldPartitionLevelStreamingDynamic>(StreamingLevel);
			const UWorldPartitionRuntimeLevelStreamingCell* Cell = WorldPartitionStreaming
				? Cast<UWorldPartitionRuntimeLevelStreamingCell>(WorldPartitionStreaming->GetWorldPartitionRuntimeCell())
				: nullptr;
			if (Cell && Cell->GetGuid() == CellGuid)
			{
				return StreamingLevel->GetLevelStreamingState() == ELevelStreamingState::LoadedVisible;
			}
		}
		return false;
	}

	bool IsActorSupportedByLevel(UWorld* World, AActor* Actor, const ULevel* Level)
	{
		if (!IsValid(World) || !IsValid(Actor) || !IsValid(Level))
		{
			return false;
		}

		//A Pawn riding a moving base (lift, vehicle) is supported by that base's Level.
		if (const APawn* Pawn = Cast<APawn>(Actor))
		{
			if (const AActor* BaseActor = APawn::GetMovementBaseActor(Pawn))
			{
				if (BaseActor->GetLevel() == Level)
				{
					return true;
				}
			}
		}

		const UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
		if (!RootPrimitive || !RootPrimitive->IsSimulatingPhysics())
		{
			return false;
		}

		//Otherwise, trace a short distance below the bounds for resting contact: 2 units of
		//overlap tolerance plus 20 units of reach, enough to catch normal physics settling
		//jitter without reaching far enough to hit unrelated geometry below a gap.
		FVector BoundsOrigin;
		FVector BoundsExtent;
		Actor->GetActorBounds(true, BoundsOrigin, BoundsExtent, false);
		const FVector Start(BoundsOrigin.X, BoundsOrigin.Y, BoundsOrigin.Z - BoundsExtent.Z + 2.0);
		const FVector End = Start - FVector(0.0, 0.0, 22.0);
		FHitResult Hit;
		const FCollisionQueryParams QueryParams(NAME_None, false, Actor);
		const FCollisionObjectQueryParams ObjectParams(
			ECC_TO_BITFIELD(ECC_WorldStatic)
			| ECC_TO_BITFIELD(ECC_WorldDynamic)
			| ECC_TO_BITFIELD(ECC_PhysicsBody));
		if (!World->LineTraceSingleByObjectType(Hit, Start, End, ObjectParams, QueryParams))
		{
			return false;
		}
		const UPrimitiveComponent* HitComponent = Hit.GetComponent();
		const AActor* SupportActor = HitComponent ? HitComponent->GetOwner() : nullptr;
		return SupportActor && SupportActor->GetLevel() == Level;
	}
}

bool UEMSWorldPartitionRuntimeSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UEMSWorldPartitionRuntimeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bDeinitializing = false;
	BindDelegates();
	BindEMSDelegate();
}

void UEMSWorldPartitionRuntimeSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (!CanOperate())
	{
		return;
	}

	//The safety pass starts even when the manager is not ready yet, because Reconcile
	//re-runs EnsureReady and is the only thing that can recover a late or failed setup.
	InWorld.GetTimerManager().SetTimer(
		ReconcileTimerHandle,
		this,
		&UEMSWorldPartitionRuntimeSubsystem::Reconcile,
		1.0f,
		true,
		1.0f);

	if (EnsureReady())
	{
		RestoreVisibleDormantRecords();
	}
}

void UEMSWorldPartitionRuntimeSubsystem::Deinitialize()
{
	bDeinitializing = true;
	bSuppressGameplayDestruction = true;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReconcileTimerHandle);
		if (ActorDestroyedHandle.IsValid())
		{
			World->RemoveOnActorDestroyedHandler(ActorDestroyedHandle);
		}
	}
	ActorDestroyedHandle.Reset();
	UnbindDelegates();
	UnbindEMSDelegate();
	LiveActorsById.Empty();
	LiveIdsByActor.Empty();
	PrimaryManager.Reset();
	Super::Deinitialize();
}

bool UEMSWorldPartitionRuntimeSubsystem::CanOperate() const
{
	const UWorld* World = GetWorld();
	return IsInGameThread()
		&& !bDeinitializing
		&& IsValid(World)
		&& !World->bIsTearingDown
		&& DoesSupportWorldType(World->WorldType)
		&& IsAuthorityNetMode(World->GetNetMode())
		&& IsValid(World->GetWorldPartition());
}

bool UEMSWorldPartitionRuntimeSubsystem::EnsureReady()
{
	if (!CanOperate())
	{
		return false;
	}
	BindDelegates();
	BindEMSDelegate();
	RefreshPrimaryManager(true);
	if (!PrimaryManager.IsValid())
	{
		return false;
	}
	if (!ActorDestroyedHandle.IsValid())
	{
		ActorDestroyedHandle = GetWorld()->AddOnActorDestroyedHandler(
			FOnActorDestroyed::FDelegate::CreateUObject(this, &UEMSWorldPartitionRuntimeSubsystem::HandleActorDestroyed));
	}
	return true;
}

void UEMSWorldPartitionRuntimeSubsystem::RefreshPrimaryManager(const bool bCreateIfMissing)
{
	UWorld* World = GetWorld();
	if (!CanOperate() || !World)
	{
		PrimaryManager.Reset();
		return;
	}

	//EnsureReady runs on the reconcile timer and on every streaming event, and the world
	//scan below is its only expensive part. A still-valid primary needs no rescan; losing
	//it invalidates the weak pointer, which is what triggers the scan again.
	if (PrimaryManager.IsValid())
	{
		return;
	}

	TArray<AEMSWorldPartitionRuntimeManager*> Managers;
	for (TActorIterator<AEMSWorldPartitionRuntimeManager> It(World); It; ++It)
	{
		AEMSWorldPartitionRuntimeManager* Manager = *It;
		if (IsValid(Manager) && Manager->GetWorld() == World)
		{
			Managers.Add(Manager);
		}
	}

	if (Managers.IsEmpty() && bCreateIfMissing)
	{
		FActorSpawnParameters Parameters;
		Parameters.OverrideLevel = World->PersistentLevel;
		Parameters.Name = TEXT("EMS_WorldPartitionRuntimeManager");
		Parameters.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AEMSWorldPartitionRuntimeManager* Created = World->SpawnActor<AEMSWorldPartitionRuntimeManager>(
			AEMSWorldPartitionRuntimeManager::StaticClass(), FTransform::Identity, Parameters);
		if (Created)
		{
			Managers.Add(Created);
		}
		else
		{
			UE_LOG(LogEMSAddonsWorldPartition, Error, TEXT("Failed to create the persistent runtime actor manager. World=%s"), *World->GetPathName());
		}
	}

	Managers.Sort(
		[](const AEMSWorldPartitionRuntimeManager& Left, const AEMSWorldPartitionRuntimeManager& Right)
		{
			return Left.GetPathName().Compare(Right.GetPathName(), ESearchCase::CaseSensitive) < 0;
		});
	PrimaryManager = Managers.IsEmpty() ? nullptr : Managers[0];
	if (Managers.Num() > 1)
	{
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Warning,
			TEXT("Several World Partition runtime managers exist. Only the deterministic primary will orchestrate actors; duplicate records are untouched. Primary=%s Count=%d"),
			*Managers[0]->GetPathName(),
			Managers.Num());
	}
}

void UEMSWorldPartitionRuntimeSubsystem::BindDelegates()
{
	if (bDelegatesBound)
	{
		return;
	}
	BeginInvisibleHandle = FLevelStreamingDelegates::OnLevelBeginMakingInvisible.AddUObject(
		this, &UEMSWorldPartitionRuntimeSubsystem::HandleLevelBeginMakingInvisible);
	StateChangedHandle = FLevelStreamingDelegates::OnLevelStreamingStateChanged.AddUObject(
		this, &UEMSWorldPartitionRuntimeSubsystem::HandleLevelStreamingStateChanged);
	bDelegatesBound = true;
}

void UEMSWorldPartitionRuntimeSubsystem::UnbindDelegates()
{
	if (!bDelegatesBound)
	{
		return;
	}
	FLevelStreamingDelegates::OnLevelBeginMakingInvisible.Remove(BeginInvisibleHandle);
	FLevelStreamingDelegates::OnLevelStreamingStateChanged.Remove(StateChangedHandle);
	BeginInvisibleHandle.Reset();
	StateChangedHandle.Reset();
	bDelegatesBound = false;
}

void UEMSWorldPartitionRuntimeSubsystem::BindEMSDelegate()
{
	UEMSObject* EMSObject = UEMSObject::Get(GetWorld());
	if (!IsValid(EMSObject) || BoundEMSObject.Get() == EMSObject)
	{
		return;
	}
	UnbindEMSDelegate();
	EMSObject->OnLevelLoaded.AddUniqueDynamic(
		this, &UEMSWorldPartitionRuntimeSubsystem::HandlePersistentLevelLoaded);
	BoundEMSObject = EMSObject;
}

void UEMSWorldPartitionRuntimeSubsystem::UnbindEMSDelegate()
{
	if (UEMSObject* EMSObject = BoundEMSObject.Get())
	{
		EMSObject->OnLevelLoaded.RemoveDynamic(
			this, &UEMSWorldPartitionRuntimeSubsystem::HandlePersistentLevelLoaded);
	}
	BoundEMSObject.Reset();
}

AActor* UEMSWorldPartitionRuntimeSubsystem::SpawnManagedActor(
	TSubclassOf<AActor> ActorClass,
	const FTransform& WorldTransform,
	const ESpawnActorCollisionHandlingMethod CollisionHandlingOverride)
{
	if (!EnsureReady())
	{
		//Expected on clients and outside World Partition worlds, so this is not a warning.
		UE_LOG(LogEMSAddonsWorldPartition, Verbose, TEXT("Ignored a runtime actor spawn on a world that is not an authoritative World Partition world."));
		return nullptr;
	}
	if (!ActorClass || !WorldTransform.IsValid())
	{
		UE_LOG(LogEMSAddonsWorldPartition, Warning, TEXT("Rejected a runtime actor spawn with no class or an invalid transform."));
		return nullptr;
	}

	//EMS refuses its own operations while a save or load is in flight, and a spawn accepted
	//here would be destroyed moments later by the load that replaces the manager's records.
	//Refusing it is the same contract, and it tells the caller instead of losing the actor.
	UEMSObject* EMSObject = UEMSObject::Get(GetWorld());
	if (EMSObject && (EMSObject->IsInitialWorldPartitionLoading() || EMSObject->IsAsyncTaskActive(false)))
	{
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Warning,
			TEXT("Rejected a runtime actor spawn because a save or load is in progress. Wait until Is Saving Or Loading is false, or until the initial World Partition load completes. Class=%s"),
			*ActorClass->GetPathName());
		return nullptr;
	}

	UClass* Class = ActorClass.Get();
	if (!Class->IsChildOf(AActor::StaticClass())
		|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
		|| !Class->ImplementsInterface(UEMSActorSaveInterface::StaticClass())
		|| Class->IsChildOf(AEMSWorldPartitionRuntimeManager::StaticClass()))
	{
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Warning,
			TEXT("Rejected a runtime actor spawn. The class must be a concrete actor implementing the EMS Actor Save Interface and must not be the runtime manager. Class=%s"),
			*Class->GetPathName());
		return nullptr;
	}

	const EMSAddonsWorldPartition::FEMSWorldPartitionCellCoverage Coverage =
		EMSAddonsWorldPartition::QueryCellCoverage(GetWorld(), WorldTransform.GetLocation());
	bool bCellIsValid = Coverage.IsVisible();
#if WITH_DEV_AUTOMATION_TESTS
	bCellIsValid = bCellIsValid || bBypassCellValidationForTests;
#endif
	if (!bCellIsValid)
	{
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Warning,
			TEXT("Rejected a runtime actor spawn because its location has no visible supported generated cell. Class=%s Location=%s"),
			*Class->GetPathName(),
			*WorldTransform.GetLocation().ToCompactString());
		return nullptr;
	}

	AEMSWorldPartitionRuntimeManager* Manager = PrimaryManager.Get();
	if (!Manager || Manager->RuntimeActorRecords.Num() >= MaxRuntimeActorRecords)
	{
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Warning,
			TEXT("Rejected a runtime actor spawn. Manager=%s Records=%d Limit=%d"),
			Manager ? *Manager->GetPathName() : TEXT("None"),
			Manager ? Manager->RuntimeActorRecords.Num() : 0,
			MaxRuntimeActorRecords);
		return nullptr;
	}

	FGuid LocalId;
	do
	{
		LocalId = FGuid::NewGuid();
	}
	while (FindRecord(LocalId) || LiveActorsById.Contains(LocalId));

	FEMSWorldPartitionRuntimeActorRecord Candidate;
	Candidate.LocalId = LocalId;
	Candidate.ActorClass = FSoftClassPath(Class);
	Candidate.SavedTransform = WorldTransform;
	Candidate.RecordVersion = FEMSWorldPartitionRuntimeActorRecord::CurrentVersion;

	const FName StableName = MakeStableActorName(LocalId);
	FActorSpawnParameters Parameters;
	Parameters.OverrideLevel = GetWorld()->PersistentLevel;
	Parameters.Name = StableName;
	Parameters.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
	Parameters.SpawnCollisionHandlingOverride = CollisionHandlingOverride;
	AActor* Actor = GetWorld()->SpawnActor<AActor>(Class, WorldTransform, Parameters);
	if (!IsValid(Actor) || Actor->GetLevel() != GetWorld()->PersistentLevel)
	{
		DiscardActor(Actor);
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Warning,
			TEXT("A runtime actor could not be spawned into the persistent level under its stable name. Class=%s StableName=%s"),
			*Class->GetPathName(),
			*StableName.ToString());
		return nullptr;
	}

	if (!SaveActorState(Actor, Candidate.ActorBinaryData))
	{
		DiscardActor(Actor);
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Warning,
			TEXT("A runtime actor's initial EMS state could not be captured, so it was not taken under management. Class=%s"),
			*Class->GetPathName());
		return nullptr;
	}

	//The spawn above ran user construction and BeginPlay, which may have added records.
	Manager = PrimaryManager.Get();
	if (!Manager || Manager->RuntimeActorRecords.Num() >= MaxRuntimeActorRecords)
	{
		DiscardActor(Actor);
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Warning,
			TEXT("A runtime actor was discarded because its own spawn callbacks removed the manager or reached the record limit. Class=%s Manager=%s Limit=%d"),
			*Class->GetPathName(),
			Manager ? *Manager->GetPathName() : TEXT("None"),
			MaxRuntimeActorRecords);
		return nullptr;
	}

	Manager->RuntimeActorRecords.Add(Candidate);
	if (!RegisterLiveActor(LocalId, Actor))
	{
		Manager->RuntimeActorRecords.RemoveAll([&LocalId](const FEMSWorldPartitionRuntimeActorRecord& Record) { return Record.LocalId == LocalId; });
		DiscardActor(Actor);
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Warning,
			TEXT("A runtime actor could not be taken under management and was discarded with its record. Class=%s StableName=%s"),
			*Class->GetPathName(),
			*StableName.ToString());
		return nullptr;
	}
	UE_LOG(
		LogEMSAddonsWorldPartition,
		Verbose,
		TEXT("Spawned a managed runtime actor. Actor=%s Cell=%s"),
		*Actor->GetPathName(),
		Coverage.VisibleCell ? *Coverage.VisibleCell->GetDebugName() : TEXT("None"));
	return Actor;
}

void UEMSWorldPartitionRuntimeSubsystem::DiscardActor(AActor* Actor)
{
	if (!IsValid(Actor))
	{
		return;
	}
	TWeakObjectPtr<AController> Controller = GetNonPlayerController(Actor);
	//Release the stable name first so a later restoration of the same record can claim it.
	const FName DisposalName(*FString::Printf(
		TEXT("EMS_WPRuntime_Discarded_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	Actor->Rename(*DisposalName.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
	const bool bPreviouslySuppressed = bSuppressGameplayDestruction;
	bSuppressGameplayDestruction = true;
	Actor->Destroy();
	bSuppressGameplayDestruction = bPreviouslySuppressed;
	DestroyOrphanedController(Controller.Get());
}

bool UEMSWorldPartitionRuntimeSubsystem::RegisterLiveActor(const FGuid& LocalId, AActor* Actor)
{
	if (!LocalId.IsValid() || !IsValid(Actor) || LiveIdsByActor.Contains(FObjectKey(Actor)))
	{
		return false;
	}
	if (const TWeakObjectPtr<AActor>* Existing = LiveActorsById.Find(LocalId))
	{
		if (Existing->IsValid() && Existing->Get() != Actor)
		{
			return false;
		}
	}
	Actor->Tags.AddUnique(EMS::SkipSaveTag);
	Actor->Tags.Remove(EMS::PersistentTag);
	LiveActorsById.Add(LocalId, Actor);
	LiveIdsByActor.Add(FObjectKey(Actor), LocalId);
	return true;
}

void UEMSWorldPartitionRuntimeSubsystem::UnregisterLiveActor(AActor* Actor)
{
	if (!Actor)
	{
		return;
	}
	const FObjectKey Key(Actor);
	if (const FGuid* LocalId = LiveIdsByActor.Find(Key))
	{
		LiveActorsById.Remove(*LocalId);
		LiveIdsByActor.Remove(Key);
	}
}

void UEMSWorldPartitionRuntimeSubsystem::HandleActorDestroyed(AActor* Actor)
{
	if (!Actor)
	{
		return;
	}
	const FGuid* FoundId = LiveIdsByActor.Find(FObjectKey(Actor));
	if (!FoundId)
	{
		return;
	}
	const FGuid LocalId = *FoundId;
	UWorld* World = GetWorld();
	const bool bGameplayDestruction = !bSuppressGameplayDestruction
		&& !bDeinitializing
		&& World
		&& !World->bIsTearingDown;
	UnregisterLiveActor(Actor);
	UE_LOG(
		LogEMSAddonsWorldPartition,
		Verbose,
		TEXT("A managed actor was destroyed. Actor=%s GameplayDestruction=%d"),
		*Actor->GetPathName(),
		bGameplayDestruction ? 1 : 0);
	if (bGameplayDestruction)
	{
		if (AEMSWorldPartitionRuntimeManager* Manager = PrimaryManager.Get())
		{
			Manager->RuntimeActorRecords.RemoveAll(
				[&LocalId](const FEMSWorldPartitionRuntimeActorRecord& Record) { return Record.LocalId == LocalId; });
		}
	}
}

bool UEMSWorldPartitionRuntimeSubsystem::SaveActorState(AActor* Actor, TArray<uint8>& OutBinary) const
{
	OutBinary.Reset();
#if WITH_DEV_AUTOMATION_TESTS
	if (bForceCaptureFailureForTests)
	{
		return false;
	}
#endif
	if (!IsValid(Actor))
	{
		return false;
	}
	UEMSObject* EMSObject = UEMSObject::Get(Actor);
	if (!IsValid(EMSObject))
	{
		return false;
	}
	FGameObjectSaveData SaveData;
	EMSObject->SaveActorToBinary(Actor, SaveData);
	return EMSAddons::PackActorSaveData(SaveData, OutBinary);
}

bool UEMSWorldPartitionRuntimeSubsystem::LoadActorState(AActor* Actor, const TArray<uint8>& Binary) const
{
	if (!IsValid(Actor))
	{
		return false;
	}
	FGameObjectSaveData SaveData;
	if (!EMSAddons::UnpackActorSaveData(Binary, SaveData))
	{
		return false;
	}
	UEMSObject* EMSObject = UEMSObject::Get(Actor);
	if (!IsValid(EMSObject))
	{
		return false;
	}
	EMSObject->LoadActorFromBinary(Actor, SaveData);
	return true;
}

bool UEMSWorldPartitionRuntimeSubsystem::CaptureActor(const FGuid& LocalId, AActor* Actor)
{
	if (!CanOperate() || !IsValid(Actor))
	{
		return false;
	}
	TArray<uint8> CapturedBinary;
	if (!SaveActorState(Actor, CapturedBinary))
	{
		UE_LOG(LogEMSAddonsWorldPartition, Warning, TEXT("Failed to capture a managed runtime actor; its previous complete record is retained. Actor=%s"), *Actor->GetPathName());
		return false;
	}
	if (!IsValid(Actor))
	{
		return false;
	}

	FEMSWorldPartitionRuntimeActorRecord* Record = FindRecordMutable(LocalId);
	if (!Record)
	{
		return false;
	}
	FEMSWorldPartitionRuntimeActorRecord Candidate = *Record;
	Candidate.LocalId = LocalId;
	Candidate.ActorClass = FSoftClassPath(Actor->GetClass());
	Candidate.SavedTransform = Actor->GetActorTransform();
	Candidate.RecordVersion = FEMSWorldPartitionRuntimeActorRecord::CurrentVersion;
	Candidate.ActorBinaryData = MoveTemp(CapturedBinary);
	Candidate.RequiredCellGuid = FGuid();
	*Record = MoveTemp(Candidate);
	return true;
}

bool UEMSWorldPartitionRuntimeSubsystem::CaptureAndRemoveActor(const FGuid& LocalId)
{
	AActor* Actor = nullptr;
	if (const TWeakObjectPtr<AActor>* Found = LiveActorsById.Find(LocalId))
	{
		Actor = Found->Get();
	}
	if (!IsValid(Actor) || !CaptureActor(LocalId, Actor))
	{
		return false;
	}
	return DestroyActorInternally(LocalId);
}

bool UEMSWorldPartitionRuntimeSubsystem::DestroyActorInternally(const FGuid& LocalId)
{
	AActor* Actor = nullptr;
	if (const TWeakObjectPtr<AActor>* Found = LiveActorsById.Find(LocalId))
	{
		Actor = Found->Get();
	}
	if (!IsValid(Actor))
	{
		LiveActorsById.Remove(LocalId);
		for (auto It = LiveIdsByActor.CreateIterator(); It; ++It)
		{
			if (It.Value() == LocalId)
			{
				It.RemoveCurrent();
			}
		}
		return true;
	}

	TWeakObjectPtr<AController> Controller = GetNonPlayerController(Actor);
	const FName OriginalName = Actor->GetFName();
	const FString OriginalPath = Actor->GetPathName();
	const bool bPreviouslySuppressed = bSuppressGameplayDestruction;
	bSuppressGameplayDestruction = true;
	const FName DisposalName(*FString::Printf(
		TEXT("EMS_WPRuntime_Disposed_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!Actor->Rename(*DisposalName.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional))
	{
		bSuppressGameplayDestruction = bPreviouslySuppressed;
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Warning,
			TEXT("Failed to release a managed actor's stable name before internal destruction; the actor remains managed for reconciliation retry. LocalId=%s StableName=%s Actor=%s"),
			*LocalId.ToString(EGuidFormats::Digits),
			*MakeStableActorName(LocalId).ToString(),
			*OriginalPath);
		return false;
	}

	bool bDestroyed = false;
#if WITH_DEV_AUTOMATION_TESTS
	if (!bForceDestroyFailureForTests)
#endif
	{
		bDestroyed = Actor->Destroy();
	}
	bDestroyed = bDestroyed || !IsValid(Actor);
	bSuppressGameplayDestruction = bPreviouslySuppressed;
	if (bDestroyed)
	{
		if (LiveActorsById.Contains(LocalId))
		{
			UnregisterLiveActor(Actor);
		}
		DestroyOrphanedController(Controller.Get());
		return true;
	}

	if (!Actor->Rename(*OriginalName.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional))
	{
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Error,
			TEXT("Internal actor destruction failed and its stable name could not be restored; ownership and the durable record are retained. LocalId=%s StableName=%s CurrentName=%s Actor=%s"),
			*LocalId.ToString(EGuidFormats::Digits),
			*MakeStableActorName(LocalId).ToString(),
			*Actor->GetName(),
			*Actor->GetPathName());
	}
	else
	{
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Warning,
			TEXT("Internal actor destruction failed; its stable name, live binding, and durable record were restored for reconciliation retry. LocalId=%s StableName=%s Actor=%s"),
			*LocalId.ToString(EGuidFormats::Digits),
			*MakeStableActorName(LocalId).ToString(),
			*Actor->GetPathName());
	}
	return false;
}

bool UEMSWorldPartitionRuntimeSubsystem::ValidateRecord(
	const FEMSWorldPartitionRuntimeActorRecord& Record,
	UClass*& OutClass) const
{
	OutClass = nullptr;
	if (Record.RecordVersion != FEMSWorldPartitionRuntimeActorRecord::CurrentVersion
		|| !Record.LocalId.IsValid()
		|| !Record.SavedTransform.IsValid())
	{
		return false;
	}
	FGameObjectSaveData SaveData;
	if (!EMSAddons::UnpackActorSaveData(Record.ActorBinaryData, SaveData))
	{
		return false;
	}
	OutClass = Record.ActorClass.ResolveClass();
	if (!OutClass)
	{
		OutClass = Record.ActorClass.TryLoadClass<AActor>();
	}
	return OutClass
		&& OutClass->IsChildOf(AActor::StaticClass())
		&& !OutClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
		&& OutClass->ImplementsInterface(UEMSActorSaveInterface::StaticClass())
		&& !OutClass->IsChildOf(AEMSWorldPartitionRuntimeManager::StaticClass());
}

//By value on purpose. Restoration runs user construction, BeginPlay, and the EMS load
//callbacks, any of which may spawn another managed actor and reallocate the manager's
//record array, so this may not hold a reference into it.
bool UEMSWorldPartitionRuntimeSubsystem::RestoreRecord(const FEMSWorldPartitionRuntimeActorRecord Record)
{
	if (!CanOperate() || LiveActorsById.Contains(Record.LocalId))
	{
		return false;
	}
	UClass* ActorClass = nullptr;
	if (!ValidateRecord(Record, ActorClass))
	{
		return false;
	}

	const FName StableName = MakeStableActorName(Record.LocalId);
	for (AActor* ExistingActor : GetWorld()->PersistentLevel->Actors)
	{
		if (IsValid(ExistingActor) && ExistingActor->GetFName() == StableName)
		{
			UE_LOG(LogEMSAddonsWorldPartition, Warning, TEXT("An unrelated actor occupies a managed stable name; the dormant record is retained. Name=%s Actor=%s"), *StableName.ToString(), *ExistingActor->GetPathName());
			return false;
		}
	}

	FActorSpawnParameters Parameters;
	Parameters.OverrideLevel = GetWorld()->PersistentLevel;
	Parameters.Name = StableName;
	Parameters.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = GetWorld()->SpawnActor<AActor>(ActorClass, Record.SavedTransform, Parameters);
	if (!IsValid(Actor) || Actor->GetLevel() != GetWorld()->PersistentLevel)
	{
		DiscardActor(Actor);
		return false;
	}
	if (!RegisterLiveActor(Record.LocalId, Actor))
	{
		DiscardActor(Actor);
		return false;
	}

	//A load callback may destroy the actor, which is the addon's own doing here.
	const bool bPreviouslySuppressed = bSuppressGameplayDestruction;
	bSuppressGameplayDestruction = true;
	const bool bLoaded = LoadActorState(Actor, Record.ActorBinaryData);
	bSuppressGameplayDestruction = bPreviouslySuppressed;
	const bool bStillBound = IsValid(Actor)
		&& LiveIdsByActor.FindRef(FObjectKey(Actor)) == Record.LocalId;
	if (!bLoaded || !bStillBound)
	{
		if (IsValid(Actor))
		{
			DestroyActorInternally(Record.LocalId);
		}
		return false;
	}
	if (FEMSWorldPartitionRuntimeActorRecord* LiveRecord = FindRecordMutable(Record.LocalId))
	{
		LiveRecord->RequiredCellGuid = FGuid();
	}
	UE_LOG(
		LogEMSAddonsWorldPartition,
		Verbose,
		TEXT("Restored a dormant managed actor. Actor=%s"),
		*Actor->GetPathName());
	return true;
}

void UEMSWorldPartitionRuntimeSubsystem::RestoreVisibleDormantRecords()
{
	AEMSWorldPartitionRuntimeManager* Manager = PrimaryManager.Get();
	if (!CanOperate() || !Manager || Manager->RuntimeActorRecords.IsEmpty())
	{
		return;
	}

	TSet<FGuid> SeenIds;
	TSet<FGuid> DuplicateIds;
	for (const FEMSWorldPartitionRuntimeActorRecord& Record : Manager->RuntimeActorRecords)
	{
		if (Record.LocalId.IsValid() && SeenIds.Contains(Record.LocalId))
		{
			DuplicateIds.Add(Record.LocalId);
		}
		SeenIds.Add(Record.LocalId);
	}

	const int32 ProcessCount = FMath::Min(Manager->RuntimeActorRecords.Num(), MaxRuntimeActorRecords);
	for (int32 Index = 0; Index < ProcessCount; ++Index)
	{
		//RestoreRecord below can grow or shrink the array through user callbacks.
		if (!Manager->RuntimeActorRecords.IsValidIndex(Index))
		{
			break;
		}
		const FEMSWorldPartitionRuntimeActorRecord& Record = Manager->RuntimeActorRecords[Index];
		if (LiveActorsById.Contains(Record.LocalId) || DuplicateIds.Contains(Record.LocalId))
		{
			continue;
		}
		if (Record.RequiredCellGuid.IsValid() && !IsCellVisible(GetWorld(), Record.RequiredCellGuid))
		{
			continue;
		}
		if (EMSAddonsWorldPartition::QueryCellCoverage(
			GetWorld(), Record.SavedTransform.GetLocation()).IsVisible())
		{
			RestoreRecord(Record);
		}
	}
}

void UEMSWorldPartitionRuntimeSubsystem::Reconcile()
{
	if (!EnsureReady())
	{
		return;
	}
	TArray<FGuid> LiveIds;
	LiveActorsById.GetKeys(LiveIds);
	for (const FGuid& LocalId : LiveIds)
	{
		AActor* Actor = LiveActorsById.FindRef(LocalId).Get();
		if (!IsValid(Actor))
		{
			LiveActorsById.Remove(LocalId);
			continue;
		}
		const EMSAddonsWorldPartition::FEMSWorldPartitionCellCoverage Coverage =
			EMSAddonsWorldPartition::QueryCellCoverage(GetWorld(), Actor->GetActorLocation());
		if (Coverage.bCovered && !Coverage.IsVisible())
		{
			UE_LOG(
				LogEMSAddonsWorldPartition,
				Verbose,
				TEXT("Reconcile made a managed actor dormant because no cell covering it is visible. Actor=%s"),
				*Actor->GetPathName());
			CaptureAndRemoveActor(LocalId);
		}
		else if (!Coverage.bCovered && GetWorld()->GetTimeSeconds() - LastUnresolvedCellWarningTime >= 10.0)
		{
			LastUnresolvedCellWarningTime = GetWorld()->GetTimeSeconds();
			UE_LOG(
				LogEMSAddonsWorldPartition,
				Warning,
				TEXT("A managed actor currently resolves to no supported generated cell. It remains live and will be retried. Actor=%s"),
				*Actor->GetPathName());
		}
	}
	RestoreVisibleDormantRecords();
}

void UEMSWorldPartitionRuntimeSubsystem::HandleManagerPreSave(AEMSWorldPartitionRuntimeManager* Manager)
{
#if WITH_DEV_AUTOMATION_TESTS
	bLastManagerPreSaveOnGameThread = IsInGameThread();
#endif
	RefreshPrimaryManager(false);
	if (!CanOperate() || Manager != PrimaryManager.Get())
	{
		return;
	}
	TArray<FGuid> LiveIds;
	LiveActorsById.GetKeys(LiveIds);
	for (const FGuid& LocalId : LiveIds)
	{
		if (AActor* Actor = LiveActorsById.FindRef(LocalId).Get())
		{
			CaptureActor(LocalId, Actor);
		}
	}
}

void UEMSWorldPartitionRuntimeSubsystem::HandleManagerPreLoad(AEMSWorldPartitionRuntimeManager* Manager)
{
	RefreshPrimaryManager(false);
	if (!CanOperate() || Manager != PrimaryManager.Get())
	{
		return;
	}
	bPrimaryManagerLoadedSincePersistentLoadCompletion = false;
	TArray<FGuid> LiveIds;
	LiveActorsById.GetKeys(LiveIds);
	if (!LiveIds.IsEmpty())
	{
		//Loud on purpose. The usual cause is a spawn that beat the initial World Partition
		//load, and from the caller's side the actors simply vanish moments after appearing.
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Warning,
			TEXT("A load is replacing the runtime actor records, so %d live managed actor(s) are being destroyed. Any of them spawned since the loaded save was written will not come back. Spawning before the initial World Partition load completes is the usual cause."),
			LiveIds.Num());
	}
	for (const FGuid& LocalId : LiveIds)
	{
		DestroyActorInternally(LocalId);
	}
}

void UEMSWorldPartitionRuntimeSubsystem::HandleManagerLoaded(AEMSWorldPartitionRuntimeManager* Manager)
{
	RefreshPrimaryManager(false);
	if (!CanOperate() || Manager != PrimaryManager.Get())
	{
		return;
	}
	bPrimaryManagerLoadedSincePersistentLoadCompletion = true;
	RestoreVisibleDormantRecords();
}

void UEMSWorldPartitionRuntimeSubsystem::HandlePersistentLevelLoaded(
	const TArray<TSoftObjectPtr<AActor>>& LoadedActors)
{
	(void)LoadedActors;
	UEMSObject* EMSObject = BoundEMSObject.Get();
	if (!CanOperate() || !EMSObject || EMSObject->GetWorld() != GetWorld())
	{
		return;
	}

	const bool bManagerLoaded = bPrimaryManagerLoadedSincePersistentLoadCompletion;
	bPrimaryManagerLoadedSincePersistentLoadCompletion = false;
	RefreshPrimaryManager(false);
	AEMSWorldPartitionRuntimeManager* Manager = PrimaryManager.Get();
	if (bManagerLoaded || !Manager)
	{
		return;
	}

	TArray<FGuid> LiveIds;
	LiveActorsById.GetKeys(LiveIds);
	TSet<FGuid> RetainedIds;
	for (const FGuid& LocalId : LiveIds)
	{
		if (!DestroyActorInternally(LocalId))
		{
			RetainedIds.Add(LocalId);
		}
	}

	Manager->RuntimeActorRecords.RemoveAll(
		[&RetainedIds](const FEMSWorldPartitionRuntimeActorRecord& Record)
		{
			return !RetainedIds.Contains(Record.LocalId);
		});
	UE_LOG(
		LogEMSAddonsWorldPartition,
		Log,
		TEXT("The persistent save contained no runtime manager callback; cleared legacy in-memory World Partition runtime state. RetainedAfterDestroyFailure=%d"),
		RetainedIds.Num());
}

bool UEMSWorldPartitionRuntimeSubsystem::IsPrimaryManager(
	const AEMSWorldPartitionRuntimeManager* Manager) const
{
	return Manager && PrimaryManager.Get() == Manager;
}

bool UEMSWorldPartitionRuntimeSubsystem::IsManagedActor(const AActor* Actor) const
{
	return Actor && LiveIdsByActor.Contains(FObjectKey(Actor));
}

void UEMSWorldPartitionRuntimeSubsystem::GetManagedActors(TArray<AActor*>& OutActors) const
{
	OutActors.Reset();
	OutActors.Reserve(LiveActorsById.Num());
	for (const TPair<FGuid, TWeakObjectPtr<AActor>>& Pair : LiveActorsById)
	{
		if (AActor* Actor = Pair.Value.Get())
		{
			OutActors.Add(Actor);
		}
	}
}

int32 UEMSWorldPartitionRuntimeSubsystem::GetManagedActorCount() const
{
	int32 Count = 0;
	for (const TPair<FGuid, TWeakObjectPtr<AActor>>& Pair : LiveActorsById)
	{
		Count += Pair.Value.IsValid() ? 1 : 0;
	}
	return Count;
}

int32 UEMSWorldPartitionRuntimeSubsystem::GetDormantActorCount() const
{
	const AEMSWorldPartitionRuntimeManager* Manager = PrimaryManager.Get();
	if (!Manager)
	{
		return 0;
	}
	int32 Count = 0;
	for (const FEMSWorldPartitionRuntimeActorRecord& Record : Manager->RuntimeActorRecords)
	{
		Count += !LiveActorsById.Contains(Record.LocalId) ? 1 : 0;
	}
	return Count;
}

FName UEMSWorldPartitionRuntimeSubsystem::MakeStableActorName(const FGuid& LocalId)
{
	return FName(*FString::Printf(TEXT("EMS_WPRuntime_%s"), *LocalId.ToString(EGuidFormats::Digits)));
}

#if WITH_DEV_AUTOMATION_TESTS
bool UEMSWorldPartitionRuntimeSubsystem::DestroyManagedActorForTesting(const AActor* Actor)
{
	if (!Actor)
	{
		return false;
	}
	const FGuid* LocalId = LiveIdsByActor.Find(FObjectKey(Actor));
	return LocalId && DestroyActorInternally(*LocalId);
}
#endif

FEMSWorldPartitionRuntimeActorRecord* UEMSWorldPartitionRuntimeSubsystem::FindRecordMutable(const FGuid& LocalId)
{
	AEMSWorldPartitionRuntimeManager* Manager = PrimaryManager.Get();
	return Manager ? Manager->RuntimeActorRecords.FindByPredicate(
		[&LocalId](const FEMSWorldPartitionRuntimeActorRecord& Record) { return Record.LocalId == LocalId; }) : nullptr;
}

const FEMSWorldPartitionRuntimeActorRecord* UEMSWorldPartitionRuntimeSubsystem::FindRecord(const FGuid& LocalId) const
{
	const AEMSWorldPartitionRuntimeManager* Manager = PrimaryManager.Get();
	return Manager ? Manager->RuntimeActorRecords.FindByPredicate(
		[&LocalId](const FEMSWorldPartitionRuntimeActorRecord& Record) { return Record.LocalId == LocalId; }) : nullptr;
}

const UWorldPartitionRuntimeLevelStreamingCell* UEMSWorldPartitionRuntimeSubsystem::GetCellFromStreamingLevel(
	const ULevelStreaming* StreamingLevel) const
{
	const UWorldPartitionLevelStreamingDynamic* WorldPartitionStreaming =
		Cast<UWorldPartitionLevelStreamingDynamic>(StreamingLevel);
	if (!WorldPartitionStreaming || WorldPartitionStreaming->GetStreamingWorld() != GetWorld())
	{
		return nullptr;
	}
	const UWorldPartitionRuntimeLevelStreamingCell* Cell =
		Cast<UWorldPartitionRuntimeLevelStreamingCell>(WorldPartitionStreaming->GetWorldPartitionRuntimeCell());
	return EMSAddonsWorldPartition::IsSupportedCell(Cell) ? Cell : nullptr;
}

void UEMSWorldPartitionRuntimeSubsystem::HandleLevelBeginMakingInvisible(
	UWorld* InWorld,
	const ULevelStreaming* StreamingLevel,
	ULevel* LoadedLevel)
{
	//Cheap rejections first. This fires for every level the world hides.
	if (InWorld != GetWorld() || LiveActorsById.IsEmpty())
	{
		return;
	}
	const UWorldPartitionRuntimeLevelStreamingCell* UnloadingCell = GetCellFromStreamingLevel(StreamingLevel);
	if (!UnloadingCell || !EnsureReady())
	{
		return;
	}

	TArray<FGuid> LiveIds;
	LiveActorsById.GetKeys(LiveIds);
	for (const FGuid& LocalId : LiveIds)
	{
		AActor* Actor = LiveActorsById.FindRef(LocalId).Get();
		if (!IsValid(Actor))
		{
			continue;
		}
		if (IsActorSupportedByLevel(GetWorld(), Actor, LoadedLevel))
		{
			UE_LOG(
				LogEMSAddonsWorldPartition,
				Verbose,
				TEXT("A managed actor was made dormant because its supporting cell is being hidden. Actor=%s Cell=%s"),
				*Actor->GetPathName(),
				*UnloadingCell->GetDebugName());
			if (CaptureAndRemoveActor(LocalId))
			{
				if (FEMSWorldPartitionRuntimeActorRecord* Record = FindRecordMutable(LocalId))
				{
					Record->RequiredCellGuid = UnloadingCell->GetGuid();
				}
			}
			continue;
		}

		//The hiding cell still reports itself visible here, so it is excluded from the
		//query: what matters is whether anything else keeps the location loaded.
		const EMSAddonsWorldPartition::FEMSWorldPartitionCellCoverage Coverage =
			EMSAddonsWorldPartition::QueryCellCoverage(GetWorld(), Actor->GetActorLocation(), UnloadingCell);
		if (Coverage.bCovered && !Coverage.IsVisible())
		{
			UE_LOG(
				LogEMSAddonsWorldPartition,
				Verbose,
				TEXT("A managed actor was made dormant because the last cell covering it is being hidden. Actor=%s Cell=%s"),
				*Actor->GetPathName(),
				*UnloadingCell->GetDebugName());
			CaptureAndRemoveActor(LocalId);
		}
	}
}

void UEMSWorldPartitionRuntimeSubsystem::HandleLevelStreamingStateChanged(
	UWorld* InWorld,
	const ULevelStreaming* StreamingLevel,
	ULevel* LoadedLevel,
	const ELevelStreamingState PreviousState,
	const ELevelStreamingState NewState)
{
	//Cheap rejections first. This fires for every streaming state change in the world.
	if (InWorld != GetWorld() || NewState != ELevelStreamingState::LoadedVisible)
	{
		return;
	}
	const UWorldPartitionRuntimeLevelStreamingCell* VisibleCell = GetCellFromStreamingLevel(StreamingLevel);
	if (VisibleCell && EnsureReady())
	{
		//Every dormant record is re-tested, not just the ones this cell covers: showing a
		//cell can only ever add coverage, and the coverage query is the authority on that.
		RestoreVisibleDormantRecords();
	}
}