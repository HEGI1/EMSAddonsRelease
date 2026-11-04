//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSWorldPartitionPlacedSubsystem.h"

#include "Components/PrimitiveComponent.h"
#include "EMSActorSaveInterface.h"
#include "EMSAddonsActorBinary.h"
#include "EMSAddonsActorNames.h"
#include "EMSAddonsWorldPartition.h"
#include "EMSData.h"
#include "EMSObject.h"
#include "EMSPhysicsStreamingGuardComponent.h"
#include "EMSTypes.h"
#include "EMSWorldPartitionActorSupport.h"
#include "EMSWorldPartitionCellResolver.h"
#include "EMSWorldPartitionPlacedComponent.h"
#include "EMSWorldPartitionRuntimeManager.h"
#include "EMSWorldPartitionRuntimeSubsystem.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Streaming/LevelStreamingDelegates.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"
#include "Subsystems/SubsystemCollection.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionLevelStreamingDynamic.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"
#include "WorldPartition/WorldPartitionRuntimeLevelStreamingCell.h"

namespace
{
	const UWorldPartitionRuntimeLevelStreamingCell* GetSupportedCell(const ULevelStreaming* Streaming)
	{
		const UWorldPartitionLevelStreamingDynamic* WPStreaming = Cast<UWorldPartitionLevelStreamingDynamic>(Streaming);
		const UWorldPartitionRuntimeLevelStreamingCell* Cell = WPStreaming
			? Cast<UWorldPartitionRuntimeLevelStreamingCell>(WPStreaming->GetWorldPartitionRuntimeCell()) : nullptr;
		return EMSAddonsWorldPartition::IsSupportedCell(Cell) ? Cell : nullptr;
	}

	bool IsRequiredCellReady(const UWorld* World, const FGuid& Id)
	{
		if (!Id.IsValid()) return true;
		const UWorldPartition* WP = World->GetWorldPartition();
		if (!WP || !WP->RuntimeHash) return false;
		bool bAnyCell = false;
		bool bFound = false;
		bool bVisible = false;
		WP->RuntimeHash->ForEachStreamingCells([&](const UWorldPartitionRuntimeCell* Cell)
		{
			if (Cell)
			{
				bAnyCell = true;
				if (Cell->GetGuid() == Id) { bFound = true; bVisible = Cell->IsVisible(); }
			}
			return true;
		});
		// Regenerated grids may no longer contain the saved support cell.
		return bAnyCell && (!bFound || bVisible);
	}

	/** Save logical gravity even if the independent guard has borrowed the flag. */
	bool SavePlacedActor(AActor* Actor, TArray<uint8>& OutBinary)
	{
		if (!IsValid(Actor)) return false;
		UEMSObject* EMSObject = UEMSObject::Get(Actor);
		if (!EMSObject) return false;
		struct FGravityRestore
		{
			TArray<TPair<TWeakObjectPtr<UPrimitiveComponent>, bool>> Flags;
			TWeakObjectPtr<const UEMSPhysicsStreamingGuardComponent> Guard;
			uint64 HoldGeneration = 0;
			~FGravityRestore()
			{
				const UEMSPhysicsStreamingGuardComponent* CurrentGuard = Guard.Get();
				if (!CurrentGuard || CurrentGuard->GetStreamingHoldGeneration() != HoldGeneration) return;
				for (const auto& Flag : Flags)
				{
					if (UPrimitiveComponent* Body = Flag.Key.Get()) Body->SetEnableGravity(Flag.Value);
				}
			}
		} Restore;
		if (const UEMSPhysicsStreamingGuardComponent* Guard = Actor->FindComponentByClass<UEMSPhysicsStreamingGuardComponent>())
		{
			Restore.Guard = Guard;
			Restore.HoldGeneration = Guard->GetStreamingHoldGeneration();
			for (UActorComponent* Component : Actor->GetComponents())
			{
				UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(Component);
				bool bGravity;
				if (Body && Guard->GetGravityBeforeStreamingHold(Body, bGravity))
				{
					Restore.Flags.Emplace(Body, bool(Body->BodyInstance.bEnableGravity));
					Body->SetEnableGravity(bGravity);
				}
			}
		}
		// User callbacks may destroy the owner and explicitly collect garbage.
		TStrongObjectPtr<AActor> ActorScope(Actor);
		FGameObjectSaveData Data;
		EMSObject->SaveActorToBinary(Actor, Data);
		return IsValid(Actor) && EMSAddons::PackActorSaveData(Data, OutBinary);
	}
}

bool UEMSWorldPartitionPlacedSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UEMSWorldPartitionPlacedSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UEMSWorldPartitionRuntimeSubsystem>();
	BeginInvisibleHandle = FLevelStreamingDelegates::OnLevelBeginMakingInvisible.AddUObject(
		this, &UEMSWorldPartitionPlacedSubsystem::HandleLevelBeginMakingInvisible);
	StateChangedHandle = FLevelStreamingDelegates::OnLevelStreamingStateChanged.AddUObject(
		this, &UEMSWorldPartitionPlacedSubsystem::HandleLevelStreamingStateChanged);
	SpawnedHandle = GetWorld()->AddOnActorPreSpawnInitialization(
		FOnActorSpawned::FDelegate::CreateUObject(this, &UEMSWorldPartitionPlacedSubsystem::HandleActorSpawned));
	DestroyedHandle = GetWorld()->AddOnActorDestroyedHandler(
		FOnActorDestroyed::FDelegate::CreateUObject(this, &UEMSWorldPartitionPlacedSubsystem::HandleActorDestroyed));
}

void UEMSWorldPartitionPlacedSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (!CanOperate()) return;
	GetManager();
	InWorld.GetTimerManager().SetTimer(ReconcileTimer, this,
		&UEMSWorldPartitionPlacedSubsystem::Reconcile, 1.0f, true, 0.001f);
}

void UEMSWorldPartitionPlacedSubsystem::Deinitialize()
{
	bDeinitializing = true;
	FLevelStreamingDelegates::OnLevelBeginMakingInvisible.Remove(BeginInvisibleHandle);
	FLevelStreamingDelegates::OnLevelStreamingStateChanged.Remove(StateChangedHandle);
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReconcileTimer);
		World->RemoveOnActorDestroyedHandler(DestroyedHandle);
		World->RemoveOnActorPreSpawnInitialization(SpawnedHandle);
	}
	if (UEMSObject* EMSObject = BoundEMSObject.Get())
	{
		EMSObject->OnLevelLoaded.RemoveDynamic(this, &UEMSWorldPartitionPlacedSubsystem::HandlePersistentLevelLoaded);
	}
	LiveActors.Empty();
	LiveIds.Empty();
	AuthoredDefaults.Empty();
	AuthoredActors.Empty();
	AuthoredIds.Empty();
	AuthoredIdsByName.Empty();
	RejectedIds.Empty();
	DeferredDormancy.Empty();
	DeferredSupportCells.Empty();
	DeferredDestroyedIds.Empty();
	PendingPlacements.Empty();
	PrimaryManager.Reset();
	BoundEMSObject.Reset();
	Super::Deinitialize();
}

bool UEMSWorldPartitionPlacedSubsystem::CanOperate() const
{
	const UWorld* World = GetWorld();
	return IsInGameThread() && !bDeinitializing && IsValid(World) && !World->bIsTearingDown
		&& DoesSupportWorldType(World->WorldType) && World->GetNetMode() != NM_Client
		&& World->GetWorldPartition() && World->GetWorldPartition()->bEnableStreaming;
}

AEMSWorldPartitionRuntimeManager* UEMSWorldPartitionPlacedSubsystem::GetManager()
{
	if (!CanOperate()) return nullptr;
	if (UEMSObject* EMSObject = UEMSObject::Get(GetWorld()))
	{
		if (BoundEMSObject.Get() != EMSObject)
		{
			if (UEMSObject* Previous = BoundEMSObject.Get())
			{
				Previous->OnLevelLoaded.RemoveDynamic(this, &UEMSWorldPartitionPlacedSubsystem::HandlePersistentLevelLoaded);
			}
			EMSObject->OnLevelLoaded.AddUniqueDynamic(this, &UEMSWorldPartitionPlacedSubsystem::HandlePersistentLevelLoaded);
			BoundEMSObject = EMSObject;
		}
	}
	UEMSWorldPartitionRuntimeSubsystem* Runtime = GetWorld()->GetSubsystem<UEMSWorldPartitionRuntimeSubsystem>();
	PrimaryManager = Runtime ? Runtime->GetPrimaryManager() : nullptr;
	return PrimaryManager.Get();
}

bool UEMSWorldPartitionPlacedSubsystem::IsEMSBusy() const
{
	UEMSObject* EMSObject = BoundEMSObject.Get();
	return bLoading || (EMSObject && (EMSObject->IsInitialWorldPartitionLoading() || EMSObject->IsAsyncTaskActive(false)));
}

FEMSWorldPartitionPlacedActorRecord* UEMSWorldPartitionPlacedSubsystem::FindRecord(const FGuid& Id)
{
	AEMSWorldPartitionRuntimeManager* Manager = PrimaryManager.Get();
	return Manager ? Manager->PlacedActorRecords.FindByPredicate(
		[&](const FEMSWorldPartitionPlacedActorRecord& Record) { return Record.State.LocalId == Id; }) : nullptr;
}

bool UEMSWorldPartitionPlacedSubsystem::RegisterPlacedActor(UEMSWorldPartitionPlacedComponent* Component)
{
	AActor* Actor = IsValid(Component) ? Component->GetOwner() : nullptr;
	if (!CanOperate() || bRestoringActor || bCapturing || !IsValid(Actor) || !Component->IsAuthoredPlacement()) return false;
	auto Reject = [&]()
	{
		PendingPlacements.Remove(Component);
		if (!LiveIds.Contains(FObjectKey(Actor))) Component->ReleaseEarlySaveExclusion();
		return false;
	};
	if (Component->bUnsupportedPlacement)
	{
		UE_LOG(LogEMSAddonsWorldPartition, Error, TEXT("Rejected placed actor authoring: unsupported map, Data Layer, Level Instance, sublevel or HLOD scope. Actor=%s"), *Actor->GetPathName());
		return Reject();
	}
#if WITH_EDITOR
	if (Actor->GetIsSpatiallyLoaded())
	{
		UE_LOG(LogEMSAddonsWorldPartition, Error, TEXT("Placed actor is still spatial. Resave it with its authored component before playing. Actor=%s"), *Actor->GetPathName());
		return Reject();
	}
#endif
	if (Actor->GetLevel() != GetWorld()->PersistentLevel
		|| !Actor->GetClass()->ImplementsInterface(UEMSActorSaveInterface::StaticClass())
		|| Actor->IsA<AEMSWorldPartitionRuntimeManager>()
		|| GetWorld()->GetSubsystem<UEMSWorldPartitionRuntimeSubsystem>()->IsManagedActor(Actor))
	{
		UE_LOG(LogEMSAddonsWorldPartition, Error, TEXT("Rejected placed actor: require non-spatial persistent-level placement, EMS Actor Save Interface, and exclusive placed ownership. Actor=%s"), *Actor->GetPathName());
		return Reject();
	}
	TInlineComponentArray<UEMSWorldPartitionPlacedComponent*> Components(Actor);
	if (Components.Num() != 1)
	{
		UE_LOG(LogEMSAddonsWorldPartition, Error, TEXT("A placed actor must have exactly one World Partition Placed Component. Actor=%s"), *Actor->GetPathName());
		return Reject();
	}
	const FGuid Id = Component->GetPlacementId();
	if (LiveActors.FindRef(Id).Get() == Actor && !PendingPlacements.Contains(Component))
	{
		PendingPlacements.Remove(Component);
		return true;
	}
	PendingPlacements.AddUnique(Component);
	if ((AuthoredActors.Contains(Id) && AuthoredActors.FindRef(Id).Get() != Actor)
		|| (LiveActors.Contains(Id) && LiveActors.FindRef(Id).Get() != Actor))
	{
		PendingPlacements.Remove(Component);
		UE_LOG(LogEMSAddonsWorldPartition, Error, TEXT("Duplicate placed identity was rejected. Resave the duplicated actor in the editor. Id=%s Actor=%s"), *Id.ToString(), *Actor->GetPathName());
		return Reject();
	}
	if (!AuthoredDefaults.Contains(Id) && AuthoredDefaults.Num() >= MaxPlacedActorRecords)
	{
		PendingPlacements.Remove(Component);
		UE_LOG(LogEMSAddonsWorldPartition, Error, TEXT("Placed actor registration exceeded the %d-record limit. Actor=%s"), MaxPlacedActorRecords, *Actor->GetPathName());
		return Reject();
	}

	if (!AuthoredDefaults.Contains(Id))
	{
		FEMSWorldPartitionPlacedActorRecord Baseline;
		Baseline.State.LocalId = Id;
		Baseline.State.ActorClass = FSoftClassPath(Actor->GetClass());
		Baseline.State.SavedTransform = Actor->GetActorTransform();
		Baseline.ActorName = Actor->GetFName();
		Baseline.ComponentName = Component->GetFName();
		Baseline.bInstanceComponent = Component->CreationMethod == EComponentCreationMethod::Instance;
		{
			TGuardValue<bool> Guard(bCapturing, true);
			if (!SavePlacedActor(Actor, Baseline.State.ActorBinaryData)) return false;
		}
		// Save callbacks can destroy the actor or invalidate the coordinator.
		if (!IsValid(Actor)) return false;
		AuthoredDefaults.Add(Id, Baseline);
		AuthoredActors.Add(Id, Actor);
		AuthoredIds.Add(FObjectKey(Actor), Id);
		AuthoredIdsByName.Add(Actor->GetFName(), Id);
		LiveActors.Add(Id, Actor);
		LiveIds.Add(FObjectKey(Actor), Id);
	}
	AEMSWorldPartitionRuntimeManager* Manager = GetManager();
	if (!Manager || IsEMSBusy()) return false;
	if (!FindRecord(Id))
	{
		if (Manager->PlacedActorRecords.Num() >= MaxPlacedActorRecords)
		{
			// Keep the authored baseline for retry, but leave no provisional placed
			// ownership or accidental EMS suppression while the array is full.
			LiveActors.Remove(Id);
			LiveIds.Remove(FObjectKey(Actor));
			Component->ReleaseEarlySaveExclusion();
			if (!Component->bReportedCapacity)
			{
				Component->bReportedCapacity = true;
				UE_LOG(LogEMSAddonsWorldPartition, Error, TEXT("Placed actor registration exceeded the %d-record limit. Actor=%s"), MaxPlacedActorRecords, *Actor->GetPathName());
			}
			return false;
		}
		Manager->PlacedActorRecords.Add(AuthoredDefaults.FindChecked(Id));
	}
	else if (bHasLoadedPlacedState)
	{
		// A disk load can complete before a queued original enrolls. Reconcile
		// that original before any dormant saved instance is allowed to spawn.
		bPendingLoadReset = true;
	}
	Component->bReportedCapacity = false;
	Actor->Tags.AddUnique(EMS::SkipSaveTag);
	Actor->Tags.Remove(EMS::PersistentTag);
	LiveActors.Add(Id, Actor);
	LiveIds.Add(FObjectKey(Actor), Id);
	PendingPlacements.Remove(Component);
	// Do not require visible coverage here: all map originals start loaded. The
	// first reconciliation after EMS loading captures any out-of-range original.
	return true;
}

bool UEMSWorldPartitionPlacedSubsystem::Capture(const FGuid& Id)
{
	AActor* Actor = LiveActors.FindRef(Id).Get();
	FEMSWorldPartitionPlacedActorRecord* Record = FindRecord(Id);
	if (!IsValid(Actor) || !Record || bCapturing) return false;
	FEMSWorldPartitionPlacedActorRecord Copy = *Record;
	const uint64 Generation = LoadGeneration;
	TGuardValue<bool> Guard(bCapturing, true);
	if (!SavePlacedActor(Actor, Copy.State.ActorBinaryData))
	{
		UE_LOG(LogEMSAddonsWorldPartition, Warning, TEXT("Placed actor capture failed; its previous complete record is retained. Id=%s"), *Id.ToString());
		return false;
	}
	Copy.State.SavedTransform = Actor->GetActorTransform();
	// Persist existing support as well as departure gating. During a disk load
	// originals are removed before deserialization, so a departing support cell
	// may have no live actor left to associate with its hide notification.
	Copy.State.RequiredCellGuid = EMSAddonsWorldPartition::GetCurrentSupportCell(GetWorld(), Actor);
	Copy.bDestroyed = false;
	// Reacquire after user callbacks: neither an array reference nor a previously
	// live identity is safe across ActorPreSave.
	Record = FindRecord(Id);
	if (Generation != LoadGeneration || !Record || !IsValid(Actor) || LiveActors.FindRef(Id).Get() != Actor) return false;
	if (const FGuid* RequiredCell = DeferredSupportCells.Find(Id))
	{
		Copy.State.RequiredCellGuid = *RequiredCell;
		DeferredDormancy.Add(Id, Copy);
	}
	*Record = MoveTemp(Copy);
	return true;
}

bool UEMSWorldPartitionPlacedSubsystem::RemoveLiveActor(const FGuid& Id)
{
	AActor* Actor = LiveActors.FindRef(Id).Get();
	if (!Actor)
	{
		LiveActors.Remove(Id);
		for (auto It = LiveIds.CreateIterator(); It; ++It) if (It.Value() == Id) It.RemoveCurrent();
		return true;
	}
	const FName OriginalName = Actor->GetFName();
	const FString DisposalName = FString::Printf(TEXT("EMS_WPPlaced_Disposed_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TGuardValue<bool> Guard(bInternalRemoval, true);
	TGuardValue<AActor*> RemovingActor(InternalRemovalActor, Actor);
	if (!Actor->Rename(*DisposalName, nullptr, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty))
	{
		UE_LOG(LogEMSAddonsWorldPartition, Warning, TEXT("Placed actor removal could not release its name; keeping the live actor and record for retry. Id=%s"), *Id.ToString());
		return false;
	}
	APawn* Pawn = Cast<APawn>(Actor);
	TWeakObjectPtr<AController> Controller = Pawn ? Pawn->GetController() : nullptr;
	if (!Actor->Destroy() && IsValid(Actor))
	{
		Actor->Rename(*OriginalName.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
		UE_LOG(LogEMSAddonsWorldPartition, Warning, TEXT("Placed actor removal failed; keeping its live binding for retry. Id=%s"), *Id.ToString());
		return false;
	}
	LiveIds.Remove(FObjectKey(Actor));
	if (LiveActors.FindRef(Id).Get() == Actor || !LiveActors.FindRef(Id).IsValid()) LiveActors.Remove(Id);
	if (Controller.IsValid() && !Controller->IsPlayerController() && !Controller->GetPawn()) Controller->Destroy();
	return true;
}

void UEMSWorldPartitionPlacedSubsystem::HandleActorDestroyed(AActor* Actor)
{
	const FGuid* Found = Actor ? LiveIds.Find(FObjectKey(Actor)) : nullptr;
	if (!Found && Actor) Found = AuthoredIds.Find(FObjectKey(Actor));
	if (!Found) return;
	const FGuid Id = *Found;
	LiveIds.Remove(FObjectKey(Actor));
	LiveActors.Remove(Id);
	if (InternalRemovalActor != Actor && CanOperate())
	{
		DeferredDormancy.Remove(Id);
		DeferredSupportCells.Remove(Id);
		if (IsEMSBusy() || !FindRecord(Id)) DeferredDestroyedIds.Add(Id);
		else if (FEMSWorldPartitionPlacedActorRecord* Record = FindRecord(Id)) Record->bDestroyed = true;
	}
}

bool UEMSWorldPartitionPlacedSubsystem::Restore(const FEMSWorldPartitionPlacedActorRecord Record)
{
	const FGuid Id = Record.State.LocalId;
	if (!CanOperate() || Record.bDestroyed || LiveActors.Contains(Id) || RejectedIds.Contains(Id)) return false;
	UClass* Class = Record.State.ActorClass.TryLoadClass<AActor>();
	FGameObjectSaveData Data;
	if (!Id.IsValid() || Record.ActorName.IsNone() || Record.ComponentName.IsNone()
		|| Record.State.RecordVersion != FEMSWorldPartitionRuntimeActorRecord::CurrentVersion
		|| !Record.State.SavedTransform.IsValid() || !Class
		|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
		|| Class->IsChildOf(AEMSWorldPartitionRuntimeManager::StaticClass())
		|| !Class->ImplementsInterface(UEMSActorSaveInterface::StaticClass())
		|| !EMSAddons::UnpackActorSaveData(Record.State.ActorBinaryData, Data))
	{
		RejectedIds.Add(Id);
		UE_LOG(LogEMSAddonsWorldPartition, Warning, TEXT("Placed actor record cannot be restored; retained until the next load. Id=%s"), *Id.ToString());
		return false;
	}
	if (!EMSAddons::ReleaseDestroyedActorName(GetWorld()->PersistentLevel, Record.ActorName)) return false;
	TGuardValue<bool> Restoring(bRestoringActor, true);
	TGuardValue<bool> Removing(bInternalRemoval, true);
	FActorSpawnParameters Parameters;
	Parameters.OverrideLevel = GetWorld()->PersistentLevel;
	Parameters.Name = Record.ActorName;
	Parameters.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Parameters.bDeferConstruction = true;
	AActor* Actor = GetWorld()->SpawnActor<AActor>(Class, Record.State.SavedTransform, Parameters);
	if (!IsValid(Actor)) return false;
	TGuardValue<AActor*> RemovingActor(InternalRemovalActor, Actor);
	TStrongObjectPtr<AActor> ActorScope(Actor);
	Actor->Tags.AddUnique(EMS::SkipSaveTag);
	Actor->Tags.Remove(EMS::PersistentTag);
	// Class-authored components already exist on native actors. Instance-authored
	// markers must be reconstructed explicitly; their ID never comes from the CDO.
	UEMSWorldPartitionPlacedComponent* Component = Actor->FindComponentByClass<UEMSWorldPartitionPlacedComponent>();
	if (!Component && Record.bInstanceComponent)
	{
		Component = NewObject<UEMSWorldPartitionPlacedComponent>(Actor, Record.ComponentName);
		Actor->AddInstanceComponent(Component);
		Component->SetRestoredIdentity(Id);
		Component->RegisterComponent();
	}
	else if (Component) Component->SetRestoredIdentity(Id);
	LiveActors.Add(Id, Actor);
	LiveIds.Add(FObjectKey(Actor), Id);
	IEMSActorSaveInterface::Execute_ActorPreLoad(Actor);
	if (IsValid(Actor)) Actor->FinishSpawning(Record.State.SavedTransform, false);
	// Blueprint construction can replace the marker, so bind its recreated copy.
	if (IsValid(Actor))
	{
		Component = Actor->FindComponentByClass<UEMSWorldPartitionPlacedComponent>();
		TInlineComponentArray<UEMSWorldPartitionPlacedComponent*> Markers(Actor);
		Component = Markers.Num() == 1 ? Markers[0] : nullptr;
		if (Component) Component->SetRestoredIdentity(Id);
	}
	UEMSObject* EMSObject = IsValid(Actor) ? UEMSObject::Get(Actor) : nullptr;
	if (!EMSObject || !Component || Actor->GetLevel() != GetWorld()->PersistentLevel)
	{
		RemoveLiveActor(Id);
		return false;
	}
	EMSObject->LoadActorFromBinary(Actor, Data, true);
	const bool bRestored = IsValid(Actor) && LiveActors.FindRef(Id).Get() == Actor;
	if (bRestored)
	{
		if (FEMSWorldPartitionPlacedActorRecord* Current = FindRecord(Id)) Current->State.RequiredCellGuid.Invalidate();
	}
	return bRestored;
}

void UEMSWorldPartitionPlacedSubsystem::ApplyAuthoredDefaults()
{
	AEMSWorldPartitionRuntimeManager* Manager = PrimaryManager.Get();
	if (!Manager) return;
	TSet<FGuid> ExistingIds;
	for (const auto& Record : Manager->PlacedActorRecords) ExistingIds.Add(Record.State.LocalId);
	for (const auto& Pair : AuthoredDefaults)
	{
		if (!ExistingIds.Contains(Pair.Key) && Manager->PlacedActorRecords.Num() < MaxPlacedActorRecords)
		{
			Manager->PlacedActorRecords.Add(Pair.Value);
			ExistingIds.Add(Pair.Key);
		}
	}
}

void UEMSWorldPartitionPlacedSubsystem::Reconcile()
{
	if (!CanOperate() || bReconciling || bCapturing || !GetManager() || IsEMSBusy()) return;
	TGuardValue<bool> Reconciling(bReconciling, true);
	ApplyAuthoredDefaults();
	ApplyDeferredChanges();
	if (!CanOperate() || !PrimaryManager.IsValid()) return;
	const auto Pending = PendingPlacements;
	for (const auto& WeakComponent : Pending)
	{
		if (UEMSWorldPartitionPlacedComponent* Component = WeakComponent.Get()) RegisterPlacedActor(Component);
		else PendingPlacements.Remove(WeakComponent);
		if (!CanOperate() || !PrimaryManager.IsValid()) return;
	}
	if (bPendingLoadReset)
	{
		TArray<FGuid> Ids;
		LiveActors.GetKeys(Ids);
		for (const FGuid& Id : Ids) RemoveLiveActor(Id);
		if (!LiveActors.IsEmpty()) return;
		bPendingLoadReset = false;
		ApplyAuthoredDefaults();
	}
	TArray<FGuid> Ids;
	LiveActors.GetKeys(Ids);
	for (const FGuid& Id : Ids)
	{
		AActor* Actor = LiveActors.FindRef(Id).Get();
		if (!Actor) { RemoveLiveActor(Id); continue; }
		const auto Coverage = EMSAddonsWorldPartition::QueryCellCoverage(GetWorld(), Actor->GetActorLocation());
		if (Coverage.bCovered && !Coverage.IsVisible() && Capture(Id)) RemoveLiveActor(Id);
	}

	// Snapshot IDs, not the record array: restore callbacks may mutate records.
	Ids.Reset();
	TSet<FGuid> DuplicateIds;
	TMap<FGuid, int32> RecordIndices;
	for (const auto& Record : PrimaryManager->PlacedActorRecords)
	{
		if (RecordIndices.Contains(Record.State.LocalId)) DuplicateIds.Add(Record.State.LocalId);
		else RecordIndices.Add(Record.State.LocalId, Ids.Num());
		Ids.Add(Record.State.LocalId);
		if (Ids.Num() >= MaxPlacedActorRecords) break;
	}
	for (const FGuid& Id : DuplicateIds)
	{
		if (!RejectedIds.Contains(Id)) UE_LOG(LogEMSAddonsWorldPartition, Warning, TEXT("Duplicate placed save identity is retained without restoration until the next load. Id=%s"), *Id.ToString());
		RejectedIds.Add(Id);
	}
	for (const FGuid& Id : Ids)
	{
		if (!PrimaryManager.IsValid()) return;
		if (LiveActors.Contains(Id) || DuplicateIds.Contains(Id) || RejectedIds.Contains(Id)) continue;
		const int32 Index = RecordIndices.FindRef(Id);
		const auto& Records = PrimaryManager->PlacedActorRecords;
		const FEMSWorldPartitionPlacedActorRecord* Record = Records.IsValidIndex(Index) && Records[Index].State.LocalId == Id
			? &Records[Index] : FindRecord(Id);
		if (!Record || Record->bDestroyed) continue;
		if (IsRequiredCellReady(GetWorld(), Record->State.RequiredCellGuid)
			&& EMSAddonsWorldPartition::QueryCellCoverage(GetWorld(), Record->State.SavedTransform.GetLocation()).IsVisible())
		{
			Restore(*Record);
		}
	}
}

void UEMSWorldPartitionPlacedSubsystem::HandleManagerPreSave(AEMSWorldPartitionRuntimeManager* Manager)
{
	if (!CanOperate() || bLoading || bPendingLoadReset || GetManager() != Manager) return;
	TArray<FGuid> Ids;
	LiveActors.GetKeys(Ids);
	for (const FGuid& Id : Ids)
	{
		if (!DeferredDormancy.Contains(Id)) Capture(Id);
	}
	ApplyDeferredChanges();
}

void UEMSWorldPartitionPlacedSubsystem::HandleManagerPreLoad(AEMSWorldPartitionRuntimeManager* Manager)
{
	if (!CanOperate() || GetManager() != Manager) return;
	++LoadGeneration;
	DeferredDormancy.Reset();
	DeferredSupportCells.Reset();
	DeferredDestroyedIds.Reset();
	bLoading = true;
	bManagerLoadedSinceCompletion = false;
	bPendingLoadReset = true;
	RejectedIds.Reset();
	TArray<FGuid> Ids;
	LiveActors.GetKeys(Ids);
	for (const FGuid& Id : Ids) RemoveLiveActor(Id);
	// Old saves have no placed array; clear explicitly before deserialization so
	// loading one cannot leave records from the previously loaded slot behind.
	if (IsValid(Manager)) Manager->PlacedActorRecords.Reset();
}

void UEMSWorldPartitionPlacedSubsystem::HandleManagerLoaded(AEMSWorldPartitionRuntimeManager* Manager)
{
	if (!CanOperate() || GetManager() != Manager) return;
	bManagerLoadedSinceCompletion = true;
	bHasLoadedPlacedState = true;
	bLoading = false;
	bPendingLoadReset = true;
	Reconcile();
}

void UEMSWorldPartitionPlacedSubsystem::HandlePersistentLevelLoaded(const TArray<TSoftObjectPtr<AActor>>& LoadedActors)
{
	(void)LoadedActors;
	if (!CanOperate() || !GetManager()) return;
	bHasLoadedPlacedState = true;
	if (!bManagerLoadedSinceCompletion)
	{
		// A legacy save without a manager means authored defaults for placed actors.
		HandleManagerPreLoad(PrimaryManager.Get());
		bLoading = false;
	}
	bManagerLoadedSinceCompletion = false;
	Reconcile();
}

bool UEMSWorldPartitionPlacedSubsystem::IsPlacedActorManaged(const AActor* Actor) const
{
	return Actor && LiveIds.Contains(FObjectKey(Actor));
}

TArray<AActor*> UEMSWorldPartitionPlacedSubsystem::GetManagedPlacedActors() const
{
	TArray<AActor*> Result;
	for (const auto& Pair : LiveActors) if (AActor* Actor = Pair.Value.Get()) Result.Add(Actor);
	return Result;
}

void UEMSWorldPartitionPlacedSubsystem::HandleLevelBeginMakingInvisible(
	UWorld* World, const ULevelStreaming* Streaming, ULevel* Level)
{
	if (World != GetWorld() || !CanOperate()) return;
	const UWorldPartitionRuntimeLevelStreamingCell* Cell = GetSupportedCell(Streaming);
	if (!Cell || !GetManager()) return;
	TArray<FGuid> Ids;
	LiveActors.GetKeys(Ids);
	for (const FGuid& Id : Ids)
	{
		AActor* Actor = LiveActors.FindRef(Id).Get();
		if (!Actor) continue;
		const bool bSupportDeparting = EMSAddonsWorldPartition::IsActorSupportedByLevel(World, Actor, Level);
		const auto Coverage = EMSAddonsWorldPartition::QueryCellCoverage(World, Actor->GetActorLocation(), Cell);
		if (!bSupportDeparting && !(Coverage.bCovered && !Coverage.IsVisible())) continue;
		const FGuid RequiredCell = bSupportDeparting ? Cell->GetGuid() : FGuid();
		if (!DeferredSupportCells.Contains(Id) || bSupportDeparting) DeferredSupportCells.Add(Id, RequiredCell);
		// Keep the first pre-departure snapshot, before gravity can move the actor.
		// Only private staging storage changes while EMS owns the manager array.
		if (!DeferredDormancy.Contains(Id) && !bCapturing && !bRestoringActor)
		{
			const FEMSWorldPartitionPlacedActorRecord* Baseline = AuthoredDefaults.Find(Id);
			if (!Baseline) continue;
			FEMSWorldPartitionPlacedActorRecord Copy = *Baseline;
			Copy.State.SavedTransform = Actor->GetActorTransform();
			Copy.State.RequiredCellGuid = RequiredCell;
			const uint64 Generation = LoadGeneration;
			TGuardValue<bool> Capturing(bCapturing, true);
			if (SavePlacedActor(Actor, Copy.State.ActorBinaryData) && Generation == LoadGeneration
				&& IsValid(Actor) && LiveActors.FindRef(Id).Get() == Actor)
			{
				DeferredDormancy.Add(Id, MoveTemp(Copy));
			}
		}
	}
	if (!bCapturing && !bRestoringActor && !IsEMSBusy()) ApplyDeferredChanges();
}

void UEMSWorldPartitionPlacedSubsystem::HandleLevelStreamingStateChanged(
	UWorld* World, const ULevelStreaming* Streaming, ULevel* Level,
	const ELevelStreamingState PreviousState, const ELevelStreamingState NewState)
{
	if (World == GetWorld() && NewState == ELevelStreamingState::LoadedVisible && GetSupportedCell(Streaming)) Reconcile();
}

void UEMSWorldPartitionPlacedSubsystem::ApplyDeferredChanges()
{
	for (auto It = DeferredDestroyedIds.CreateIterator(); It; ++It)
	{
		if (FEMSWorldPartitionPlacedActorRecord* Record = FindRecord(*It))
		{
			Record->bDestroyed = true;
			It.RemoveCurrent();
		}
	}
	TArray<FGuid> Ids;
	DeferredSupportCells.GetKeys(Ids);
	for (const FGuid& Id : Ids)
	{
		// Reentrant capture finishes on the same game-thread stack and stages its
		// complete binary. A failed capture remains pending for a guarded retry.
		if (!DeferredDormancy.Contains(Id)) Capture(Id);
		const FEMSWorldPartitionPlacedActorRecord* Pending = DeferredDormancy.Find(Id);
		FEMSWorldPartitionPlacedActorRecord* Current = FindRecord(Id);
		if (!Pending || !Current) continue;
		*Current = *Pending;
		if (RemoveLiveActor(Id))
		{
			DeferredDormancy.Remove(Id);
			DeferredSupportCells.Remove(Id);
		}
	}
	// Removal callbacks can destroy another placement while EMS is active.
	// Commit those deaths before pre-save hands the array to serialization.
	for (auto It = DeferredDestroyedIds.CreateIterator(); It; ++It)
	{
		if (FEMSWorldPartitionPlacedActorRecord* Record = FindRecord(*It))
		{
			Record->bDestroyed = true;
			It.RemoveCurrent();
		}
	}
}

void UEMSWorldPartitionPlacedSubsystem::HandleActorSpawned(AActor* Actor)
{
	if (!CanOperate() || bRestoringActor || !IsValid(Actor)
		|| Actor->GetLevel() != GetWorld()->PersistentLevel || !IsEMSBusy()) return;
	const FGuid* Id = AuthoredIdsByName.Find(Actor->GetFName());
	const FEMSWorldPartitionPlacedActorRecord* Baseline = Id ? AuthoredDefaults.Find(*Id) : nullptr;
	if (!Baseline || Baseline->State.ActorClass != FSoftClassPath(Actor->GetClass())) return;
	// Ordinary EMS may still contain a legacy independent runtime record. Its
	// pre-initialization spawn must not steal the authored placement's name or run load /
	// construction callbacks. Internally recreated placed actors bypass this hook.
	const FName Name = Actor->GetFName();
	TGuardValue<bool> Removing(bInternalRemoval, true);
	TGuardValue<AActor*> RemovingActor(InternalRemovalActor, Actor);
	Actor->Tags.AddUnique(EMS::SkipSaveTag);
	if (Actor->Destroy())
	{
		UE_LOG(LogEMSAddonsWorldPartition, Warning,
			TEXT("Rejected an independent EMS copy of an authored placed actor; placed records and authored defaults retain ownership. Actor=%s"), *Name.ToString());
	}
	else
	{
		UE_LOG(LogEMSAddonsWorldPartition, Error,
			TEXT("Could not remove an independent EMS copy of placed actor %s; its name remains reserved and placed restoration will retry."), *Name.ToString());
	}
}
