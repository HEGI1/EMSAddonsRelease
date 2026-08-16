//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSGeometryCollectionActor.h"

#include "Components/BillboardComponent.h"
#include "EMSAddonsAuthority.h"
#include "EMSAddonsGameThread.h"
#include "EMSAddonsGeometry.h"
#include "EMSAddonsHash.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollectionProxyData.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "TimerManager.h"

#define LOCTEXT_NAMESPACE "EMSGeometryCollectionActor"

namespace
{
	using EMSAddons::Hash::HashValue;

	void HashTransform(uint64& Hash, const FTransform3f& Transform)
	{
		const FVector3f Translation = Transform.GetTranslation();
		const FQuat4f Rotation = Transform.GetRotation();
		const FVector3f Scale = Transform.GetScale3D();

		HashValue(Hash, Translation.X);
		HashValue(Hash, Translation.Y);
		HashValue(Hash, Translation.Z);
		HashValue(Hash, Rotation.X);
		HashValue(Hash, Rotation.Y);
		HashValue(Hash, Rotation.Z);
		HashValue(Hash, Rotation.W);
		HashValue(Hash, Scale.X);
		HashValue(Hash, Scale.Y);
		HashValue(Hash, Scale.Z);
	}
}

AEMSGeometryCollectionActor::AEMSGeometryCollectionActor()
{
	PrimaryActorTick.bCanEverTick = false;
	GeometryCollectionComponent =
		CreateDefaultSubobject<UGeometryCollectionComponent>(
			TEXT("GeometryCollectionComponent"));
	SetRootComponent(GeometryCollectionComponent);
	LastRestoreResult = FEMSAddonResult::Success();

#if WITH_EDITORONLY_DATA
	GeometrySprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(
		TEXT("GeometrySprite"));
	if (GeometrySprite)
	{
		static ConstructorHelpers::FObjectFinder<UTexture2D> GeometrySpriteTexture(
			TEXT("/Engine/EditorResources/S_Solver"));
		if (GeometrySpriteTexture.Succeeded())
		{
			GeometrySprite->SetSprite(GeometrySpriteTexture.Object);
		}

		GeometrySprite->SpriteInfo.Category = TEXT("EasyMultiSave");
		GeometrySprite->SpriteInfo.DisplayName =
			LOCTEXT("EMSSpriteCategory", "Easy Multi Save");
		GeometrySprite->bIsScreenSizeScaled = true;
		GeometrySprite->SetupAttachment(GeometryCollectionComponent);
	}
#endif
}

bool AEMSGeometryCollectionActor::CanAccessGeometryState(FText& OutReason) const
{
	if (!IsInGameThread())
	{
		OutReason = LOCTEXT("GeometryGameThreadOnly", "Geometry Collection save state must be accessed on the game thread.");
		return false;
	}

	if (HasAnyFlags(RF_ClassDefaultObject) || bIsEndingPlay)
	{
		OutReason = LOCTEXT("GeometryUnavailable", "The Geometry Collection component is unavailable or ending play.");
		return false;
	}

	if (!EMSAddons::HasPersistenceAuthority(this, GetGeometryNetMode()))
	{
		OutReason = LOCTEXT("GeometryAuthorityOnly", "Only the authoritative actor may capture or restore persistent Geometry Collection state. Persistent state is server-authoritative and is never captured or restored on a client.");
		return false;
	}

	if (!GeometryCollectionComponent->GetRestCollection())
	{
		OutReason = LOCTEXT("GeometryMissingAsset", "The component has no Rest Collection asset.");
		return false;
	}

	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> RestData =
		GeometryCollectionComponent->GetRestCollection()->GetGeometryCollection();
	if (!RestData || RestData->Transform.Num() == 0)
	{
		OutReason = LOCTEXT("GeometryMissingRestData", "The Rest Collection has no transform hierarchy.");
		return false;
	}

	OutReason = FText::GetEmpty();
	return true;
}

int64 AEMSGeometryCollectionActor::ComputeCurrentHierarchyHash() const
{
	FText Reason;
	if (!CanAccessGeometryState(Reason))
	{
		return 0;
	}

	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> RestData =
		GeometryCollectionComponent->GetRestCollection()->GetGeometryCollection();
	if (!RestData)
	{
		return 0;
	}

	uint64 Hash = EMSAddons::Hash::OffsetBasis;
	const int32 TransformCount = RestData->Transform.Num();
	HashValue(Hash, TransformCount);

	for (int32 Index = 0; Index < TransformCount; ++Index)
	{
		HashValue(Hash, RestData->Parent[Index]);
		HashTransform(Hash, RestData->Transform[Index]);

		const int32 SimulationType = RestData->SimulationType.IsValidIndex(Index)
			? RestData->SimulationType[Index]
			: INDEX_NONE;
		HashValue(Hash, SimulationType);
	}

	return static_cast<int64>(Hash);
}

bool AEMSGeometryCollectionActor::CaptureGeometryCollectionState()
{
	FText Reason;
	if (!CanAccessGeometryState(Reason))
	{
		LogRestoreIssue(Reason);
		return false;
	}

	const FGeometryDynamicCollection* CurrentDynamicCollection =
		GetPersistenceDynamicCollection();
	if (!CurrentDynamicCollection)
	{
		LogRestoreIssue(LOCTEXT("GeometryMissingDynamicCapture", "The game-thread dynamic collection is not initialized."));
		return false;
	}

	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> RestData =
		GeometryCollectionComponent->GetRestCollection()->GetGeometryCollection();
	const int32 TransformCount = RestData ? RestData->Transform.Num() : 0;
	if (TransformCount <= 0
		|| TransformCount > MaxPersistedTransforms
		|| CurrentDynamicCollection->GetNumTransforms() != TransformCount)
	{
		LogRestoreIssue(LOCTEXT("GeometryCaptureCountMismatch", "The transform count is invalid, exceeds the configured limit, or differs between the dynamic and Rest Collections."));
		return false;
	}

	FEMSGeometryCollectionState CapturedState;
	CapturedState.bHasValidData = true;
	CapturedState.RestCollectionAsset = FSoftObjectPath(GeometryCollectionComponent->GetRestCollection());
	CapturedState.HierarchyHash = ComputeCurrentHierarchyHash();
	CapturedState.bWasRootBroken = GeometryCollectionComponent->IsRootBroken();
	CapturedState.PieceTransforms.Reserve(TransformCount);

	for (int32 Index = 0; Index < TransformCount; ++Index)
	{
		CapturedState.PieceTransforms.Add(FTransform(CurrentDynamicCollection->GetTransform(Index)));

		const int32 AuthoredParent = RestData->Parent[Index];
		if (AuthoredParent != INDEX_NONE
			&& (!CurrentDynamicCollection->GetHasParent(Index)
				|| CurrentDynamicCollection->GetParent(Index) != AuthoredParent))
		{
			CapturedState.BrokenIndices.Add(Index);
		}
	}

	PersistedState = MoveTemp(CapturedState);
	LastRestoreResult = FEMSAddonResult::Success();
	return true;
}

bool AEMSGeometryCollectionActor::ValidateStateInternal(
	FText& OutReason,
	const bool bRequireDynamicCollection) const
{
	if (!CanAccessGeometryState(OutReason))
	{
		return false;
	}

	if (!PersistedState.bHasValidData)
	{
		OutReason = LOCTEXT("GeometryEmptyState", "No valid Geometry Collection state has been captured or loaded.");
		return false;
	}

	if (PersistedState.Version != FEMSGeometryCollectionState::CurrentVersion)
	{
		OutReason = FText::Format(
			LOCTEXT("GeometryUnsupportedVersion", "Unsupported Geometry Collection state version {0}."),
			FText::AsNumber(PersistedState.Version));
		return false;
	}

	const FSoftObjectPath CurrentAsset(GeometryCollectionComponent->GetRestCollection());
	if (AssetMismatchPolicy == EEMSGeometryAssetMismatchPolicy::Reject
		&& PersistedState.RestCollectionAsset != CurrentAsset)
	{
		OutReason = LOCTEXT("GeometryAssetMismatch", "The saved and current Rest Collection assets differ.");
		return false;
	}

	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> RestData =
		GeometryCollectionComponent->GetRestCollection()->GetGeometryCollection();
	const int32 CurrentCount = RestData ? RestData->Transform.Num() : 0;
	const int32 SavedCount = PersistedState.PieceTransforms.Num();
	if (SavedCount <= 0
		|| SavedCount > MaxPersistedTransforms
		|| SavedCount != CurrentCount)
	{
		OutReason = LOCTEXT("GeometryTransformCountMismatch", "The saved and current transform counts differ.");
		return false;
	}

	if (PersistedState.HierarchyHash == 0
		|| PersistedState.HierarchyHash != ComputeCurrentHierarchyHash())
	{
		OutReason = LOCTEXT("GeometryHierarchyMismatch", "The Rest Collection hierarchy or authored transforms changed.");
		return false;
	}

	// FTransform::IsValid rejects every non-finite component and requires a
	// normalized rotation, so no separate NaN check is needed.
	for (const FTransform& Transform : PersistedState.PieceTransforms)
	{
		if (!Transform.IsValid())
		{
			OutReason = LOCTEXT("GeometryInvalidTransform", "The saved state contains an invalid piece transform.");
			return false;
		}
	}

	TSet<int32> SeenBrokenIndices;
	for (const int32 BrokenIndex : PersistedState.BrokenIndices)
	{
		if (!RestData->Parent.IsValidIndex(BrokenIndex)
			|| RestData->Parent[BrokenIndex] == INDEX_NONE
			|| SeenBrokenIndices.Contains(BrokenIndex))
		{
			OutReason = LOCTEXT("GeometryInvalidBrokenIndex", "The saved state contains an invalid or duplicate broken-parent index.");
			return false;
		}
		SeenBrokenIndices.Add(BrokenIndex);
	}

	if (bRequireDynamicCollection)
	{
		const FGeometryDynamicCollection* CurrentDynamicCollection =
			GetPersistenceDynamicCollection();
		if (!CurrentDynamicCollection
			|| CurrentDynamicCollection->GetNumTransforms() != CurrentCount)
		{
			OutReason = LOCTEXT("GeometryDynamicUnavailable", "The game-thread dynamic collection is not ready.");
			return false;
		}
	}

	OutReason = FText::GetEmpty();
	return true;
}

bool AEMSGeometryCollectionActor::ValidateGeometryCollectionState(
	FText& OutReason) const
{
	return ValidateStateInternal(OutReason, true);
}

bool AEMSGeometryCollectionActor::ApplyGameThreadState(FText& OutReason)
{
	if (!ValidateStateInternal(OutReason, true))
	{
		return false;
	}

	TArray<FTransform> RestoredTransforms = PersistedState.PieceTransforms;
	GeometryCollectionComponent->SetRestState(MoveTemp(RestoredTransforms));
	GeometryCollectionComponent->SetInitialClusterBreaks(PersistedState.BrokenIndices);
	GeometryCollectionComponent->RecreatePhysicsState();

	const int32 SavedCount = PersistedState.PieceTransforms.Num();
	FGeometryDynamicCollection* CurrentDynamicCollection =
		GetPersistenceDynamicCollection();
	if (!CurrentDynamicCollection
		|| CurrentDynamicCollection->GetNumTransforms() != SavedCount)
	{
		OutReason = LOCTEXT("GeometryDynamicLost", "The dynamic collection was unavailable after physics-state recreation.");
		return false;
	}

	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> RestData =
		GeometryCollectionComponent->GetRestCollection()->GetGeometryCollection();
	const TSet<int32> BrokenSet(PersistedState.BrokenIndices);
	for (int32 Index = 0; Index < SavedCount; ++Index)
	{
		CurrentDynamicCollection->SetTransform(
			Index,
			FTransform3f(PersistedState.PieceTransforms[Index]));
		CurrentDynamicCollection->SetHasParent(
			Index,
			RestData->Parent[Index] != INDEX_NONE && !BrokenSet.Contains(Index));
	}
	CurrentDynamicCollection->MakeDirty();

	RefreshGeometryRendering();
	bGameThreadStateApplied = true;
	OutReason = FText::GetEmpty();
	return true;
}

bool AEMSGeometryCollectionActor::SynchronizePhysicsThreadState()
{
	FGeometryCollectionPhysicsProxy* CurrentPhysicsProxy = GeometryCollectionComponent->GetPhysicsProxy();
	FPhysScene* PhysicsScene = GeometryCollectionComponent->GetInnerChaosScene();
	if (!CurrentPhysicsProxy || !PhysicsScene)
	{
		return false;
	}

	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> RestData =
		GeometryCollectionComponent->GetRestCollection() ? GeometryCollectionComponent->GetRestCollection()->GetGeometryCollection() : nullptr;
	if (!RestData)
	{
		return false;
	}

	const TArray<FTransform>& SavedTransforms = PersistedState.PieceTransforms;
	const TArray<int32>& SavedBrokenIndices = PersistedState.BrokenIndices;
	const TArray<int32> AuthoredParents(RestData->Parent.GetData(), RestData->Parent.Num());

	// ExecuteWrite takes a TFunctionRef and runs the callable inline under the
	// scene write lock, so everything can be captured by reference.
	bool bPhysicsCollectionUpdated = false;
	const bool bAcquiredWriteAccess = FPhysicsCommand::ExecuteWrite(
		PhysicsScene,
		[
			CurrentPhysicsProxy,
			&SavedTransforms,
			&SavedBrokenIndices,
			&AuthoredParents,
			&bPhysicsCollectionUpdated
		]()
		{
			FGeometryDynamicCollection& PhysicsCollection =
				CurrentPhysicsProxy->GetPhysicsCollection();
			if (PhysicsCollection.GetNumTransforms() != SavedTransforms.Num())
			{
				return;
			}

			const TSet<int32> BrokenSet(SavedBrokenIndices);
			for (int32 Index = 0; Index < SavedTransforms.Num(); ++Index)
			{
				PhysicsCollection.SetTransform(
					Index,
					FTransform3f(SavedTransforms[Index]));
				PhysicsCollection.SetHasParent(
					Index,
					AuthoredParents.IsValidIndex(Index)
						&& AuthoredParents[Index] != INDEX_NONE
						&& !BrokenSet.Contains(Index));
			}
			PhysicsCollection.MakeDirty();
			bPhysicsCollectionUpdated = true;
		});

	return bAcquiredWriteAccess && bPhysicsCollectionUpdated;
}

void AEMSGeometryCollectionActor::RefreshGeometryRendering()
{
	GeometryCollectionComponent->ForceBrokenForCustomRenderer(PersistedState.IsFractured());
	GeometryCollectionComponent->RefreshCustomRenderer();
	GeometryCollectionComponent->RefreshRootProxies();
	GeometryCollectionComponent->UpdateBounds();
	GeometryCollectionComponent->MarkRenderTransformDirty();
	GeometryCollectionComponent->MarkRenderDynamicDataDirty();
	GeometryCollectionComponent->MarkRenderStateDirty();
}

bool AEMSGeometryCollectionActor::RestoreGeometryCollectionState()
{
	FText Reason;
	if (!ValidateStateInternal(Reason, false))
	{
		FailRestore(Reason);
		return false;
	}

	CancelPendingRestore(false);
	RestoreRetryCount = 0;
	bGameThreadStateApplied = false;
	RestorePhase = EEMSAddonRestorePhase::Pending;
	LastRestoreResult = FEMSAddonResult(
		EEMSAddonResultCode::Pending,
		LOCTEXT("GeometryRestorePending", "Geometry Collection restoration is pending."));
	AttemptRestore();
	return RestorePhase != EEMSAddonRestorePhase::Failed
		&& RestorePhase != EEMSAddonRestorePhase::Canceled;
}

void AEMSGeometryCollectionActor::AttemptRestore()
{
	if (bIsEndingPlay)
	{
		CancelPendingRestore(true);
		return;
	}

	if (!bGameThreadStateApplied)
	{
		FText Reason;
		if (!ValidateStateInternal(Reason, false))
		{
			FailRestore(Reason);
			return;
		}

		if (!IsGeometryRuntimeReadyForRestore())
		{
			ScheduleRestoreRetry();
			return;
		}

		if (RequiresPhysicsProxySynchronization()
			&& !IsPhysicsRuntimeReadyForRestore())
		{
			ScheduleRestoreRetry();
			return;
		}

		if (!ApplyGameThreadState(Reason))
		{
			if (!GetPersistenceDynamicCollection())
			{
				ScheduleRestoreRetry();
			}
			else
			{
				FailRestore(Reason);
			}
			return;
		}
	}

	if (RequiresPhysicsProxySynchronization()
		&& !SynchronizePhysicsThreadState())
	{
		MarkRestoreUnsafeAndFail(LOCTEXT(
			"GeometryPhysicsSyncFailedAfterMutation",
			"The Chaos physics collection could not be synchronized after game-thread state was applied. Simulation was disabled to prevent divergent state."));
		return;
	}

	FinalizeRestore();
}

void AEMSGeometryCollectionActor::MarkRestoreUnsafeAndFail(
	const FText& Reason)
{
	GeometryCollectionComponent->SetSimulatePhysics(false);
	bGameThreadStateApplied = false;
	FailRestore(Reason);
}

void AEMSGeometryCollectionActor::ScheduleRestoreRetry()
{
	if (++RestoreRetryCount > FMath::Max(1, MaxRestoreRetries))
	{
		FailRestore(LOCTEXT("GeometryRestoreTimeout", "Geometry Collection restoration timed out while waiting for runtime resources."));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		FailRestore(LOCTEXT("GeometryRestoreNoWorld", "Geometry Collection restoration cannot retry without a valid world."));
		return;
	}

	RestorePhase = EEMSAddonRestorePhase::Pending;
	World->GetTimerManager().ClearTimer(RestoreRetryTimer);
	RestoreRetryTimer = World->GetTimerManager().SetTimerForNextTick(
		this,
		&AEMSGeometryCollectionActor::AttemptRestore);
}

void AEMSGeometryCollectionActor::FinalizeRestore()
{
	RefreshGeometryRendering();
	RestorePhase = EEMSAddonRestorePhase::Complete;
	LastRestoreResult = FEMSAddonResult::Success();
}

void AEMSGeometryCollectionActor::FailRestore(const FText& Reason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RestoreRetryTimer);
	}

	RestorePhase = EEMSAddonRestorePhase::Failed;
	LastRestoreResult = FEMSAddonResult(
		EEMSAddonResultCode::RestoreFailed,
		Reason);
	LogRestoreIssue(Reason);
}

void AEMSGeometryCollectionActor::CancelPendingRestore(
	const bool bReportCanceled)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RestoreRetryTimer);
	}

	if (bReportCanceled && IsGeometryRestorePending())
	{
		RestorePhase = EEMSAddonRestorePhase::Canceled;
		LastRestoreResult = FEMSAddonResult(
			EEMSAddonResultCode::Canceled,
			LOCTEXT("GeometryRestoreCanceled", "Geometry Collection restoration was canceled during teardown."));
	}
}

bool AEMSGeometryCollectionActor::IsGeometryRestorePending() const
{
	return RestorePhase == EEMSAddonRestorePhase::Pending;
}

EEMSAddonRestorePhase AEMSGeometryCollectionActor::GetGeometryRestorePhase() const
{
	return RestorePhase;
}

FEMSAddonResult AEMSGeometryCollectionActor::GetGeometryRestoreResult() const
{
	return LastRestoreResult;
}

/**
 * A client has nothing to do here and is not at fault for it.
 *
 * The authority rule is also enforced inside capture and restore, but reaching
 * it through those reports a failed restore and logs a warning for what is the
 * designed outcome on every client, every load.
 */
void AEMSGeometryCollectionActor::SkipPersistenceWithoutAuthority()
{
	RestorePhase = EEMSAddonRestorePhase::Idle;
	LastRestoreResult = FEMSAddonResult(
		EEMSAddonResultCode::Skipped,
		LOCTEXT("GeometrySkippedWithoutAuthority", "Geometry Collection state is server-authoritative and is neither captured nor restored here."));
}

void AEMSGeometryCollectionActor::ActorPreSave_Implementation()
{
	EMSAddons::RunOnGameThread(
		[this]()
		{
			// The net mode is read on the game thread with everything else.
			if (EMSAddons::HasPersistenceAuthority(this, GetGeometryNetMode()))
			{
				CaptureGeometryCollectionState();
			}
		});
}

void AEMSGeometryCollectionActor::ActorPreLoad_Implementation()
{
	if (!EMSAddons::HasPersistenceAuthority(this, GetGeometryNetMode()))
	{
		SkipPersistenceWithoutAuthority();
		return;
	}

	CancelPendingRestore(false);
	RestorePhase = EEMSAddonRestorePhase::Idle;
	LastRestoreResult = FEMSAddonResult(
		EEMSAddonResultCode::Pending,
		LOCTEXT("GeometryWaitingForData", "Waiting for Geometry Collection save data."));
}

void AEMSGeometryCollectionActor::ActorLoaded_Implementation()
{
	if (!EMSAddons::HasPersistenceAuthority(this, GetGeometryNetMode()))
	{
		SkipPersistenceWithoutAuthority();
		return;
	}

	RestoreGeometryCollectionState();
}

bool AEMSGeometryCollectionActor::IsEMSAddonRestoreComplete() const
{
	return !IsGeometryRestorePending();
}

FEMSAddonResult AEMSGeometryCollectionActor::GetEMSAddonRestoreResult() const
{
	return LastRestoreResult;
}

void AEMSGeometryCollectionActor::BeginPlay()
{
	// A sublevel that is hidden and shown again routes End Play and then Begin
	// Play on the same actor, so this has to clear or the actor can never capture
	// or restore again for the rest of the session.
	bIsEndingPlay = false;
	Super::BeginPlay();
}

void AEMSGeometryCollectionActor::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	bIsEndingPlay = true;
	CancelPendingRestore(true);
	Super::EndPlay(EndPlayReason);
}

void AEMSGeometryCollectionActor::Destroyed()
{
	bIsEndingPlay = true;
	CancelPendingRestore(true);
	Super::Destroyed();
}

bool AEMSGeometryCollectionActor::IsGeometryRuntimeReadyForRestore() const
{
	return GetPersistenceDynamicCollection() != nullptr;
}

bool AEMSGeometryCollectionActor::RequiresPhysicsProxySynchronization() const
{
	return GeometryCollectionComponent->ShouldCreatePhysicsState();
}

bool AEMSGeometryCollectionActor::IsPhysicsRuntimeReadyForRestore() const
{
	AEMSGeometryCollectionActor* MutableThis =
		const_cast<AEMSGeometryCollectionActor*>(this);
	return MutableThis->GeometryCollectionComponent->GetPhysicsProxy() != nullptr
		&& MutableThis->GeometryCollectionComponent->GetInnerChaosScene() != nullptr;
}

FGeometryDynamicCollection*
AEMSGeometryCollectionActor::GetPersistenceDynamicCollection()
{
	return GeometryCollectionComponent->GetDynamicCollection();
}

const FGeometryDynamicCollection*
AEMSGeometryCollectionActor::GetPersistenceDynamicCollection() const
{
	return GeometryCollectionComponent->GetDynamicCollection();
}

void AEMSGeometryCollectionActor::LogRestoreIssue(const FText& Reason) const
{
	const UGeometryCollection* CurrentRestCollection = GeometryCollectionComponent->GetRestCollection();
	const int32 CurrentCount = CurrentRestCollection && CurrentRestCollection->GetGeometryCollection()
		? CurrentRestCollection->GetGeometryCollection()->Transform.Num()
		: 0;

	// The current hierarchy hash is deliberately not recomputed here. It walks
	// the whole Rest Collection, and this runs on every warning.
	UE_LOG(
		LogEMSAddonsGeometry,
		Warning,
		TEXT("EMS Geometry operation skipped or failed. Actor=%s Component=%s Asset=%s StateVersion=%d SavedCount=%d CurrentCount=%d SavedHash=%lld Phase=%d Reason=%s"),
		*GetPathName(),
		*GeometryCollectionComponent->GetPathName(),
		CurrentRestCollection ? *CurrentRestCollection->GetPathName() : TEXT("<none>"),
		PersistedState.Version,
		PersistedState.PieceTransforms.Num(),
		CurrentCount,
		PersistedState.HierarchyHash,
		static_cast<int32>(RestorePhase),
		*Reason.ToString());
}

#undef LOCTEXT_NAMESPACE
