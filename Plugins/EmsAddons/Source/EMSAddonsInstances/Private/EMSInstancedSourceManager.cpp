//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSInstancedSourceManager.h"

#include "Components/BillboardComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "CoreGlobals.h"
#include "EMSAddonsAuthority.h"
#include "EMSAddonsGameThread.h"
#include "EMSAddonsInstances.h"
#include "EMSActors.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Engine/LevelStreaming.h"
#include "Streaming/LevelStreamingDelegates.h"
#include "EngineUtils.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "WorldPartition/ActorInstanceGuids.h"

#define LOCTEXT_NAMESPACE "EMSInstancedSourceManager"

namespace
{
	/**
	 * Whether two transforms describe the same stored instance.
	 *
	 * Instance transforms live in a float matrix, so a saved double transform
	 * written back and read again is not bitwise identical, and the rotation is
	 * re-derived from the matrix rather than round-tripped. The canonical key
	 * quantizes that value, and any quantization has edges, so a drift far too
	 * small to see can still land one step away and read as a different instance.
	 *
	 * The location tolerance therefore scales with distance from the origin,
	 * because that is what float storage does: the representable step is about
	 * 0.001 units at 10,000 out and about 0.06 at 1,000,000. It is capped at half
	 * the tenth-of-a-unit granularity the canonical key itself uses, so this is
	 * never less discriminating than the key it backs up - two instances the key
	 * can tell apart are never merged here.
	 */
	bool IsSameStoredInstance(const FTransform& Left, const FTransform& Right)
	{
		constexpr double FloatRelativeStep = 4.0e-6;
		constexpr double MinLocationTolerance = 0.001;
		constexpr double MaxLocationTolerance = 0.05;
		constexpr double RotationTolerance = 1.0e-4;
		constexpr double ScaleTolerance = 1.0e-4;

		const FVector LeftLocation = Left.GetLocation();
		const double Magnitude = FMath::Max3(
			FMath::Abs(LeftLocation.X),
			FMath::Abs(LeftLocation.Y),
			FMath::Abs(LeftLocation.Z));
		const double LocationTolerance = FMath::Clamp(
			Magnitude * FloatRelativeStep,
			MinLocationTolerance,
			MaxLocationTolerance);

		return LeftLocation.Equals(Right.GetLocation(), LocationTolerance)
			&& Left.GetRotation().Equals(Right.GetRotation(), RotationTolerance)
			&& Left.GetScale3D().Equals(Right.GetScale3D(), ScaleTolerance);
	}

	bool CustomDataEqual(const TArray<float>& Left, const TArray<float>& Right)
	{
		if (Left.Num() != Right.Num())
		{
			return false;
		}

		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			if (!FMath::IsNearlyEqual(Left[Index], Right[Index]))
			{
				return false;
			}
		}
		return true;
	}

	bool IsValidCustomData(const TArray<float>& CustomData, const int32 ExpectedCount)
	{
		if (CustomData.Num() != ExpectedCount)
		{
			return false;
		}

		for (const float Value : CustomData)
		{
			if (!FMath::IsFinite(Value))
			{
				return false;
			}
		}
		return true;
	}
}

AEMSInstancedSourceManager::AEMSInstancedSourceManager()
{
	PrimaryActorTick.bCanEverTick = false;
	LastRestoreResult = FEMSAddonResult::Success();
	LastCaptureResult = FEMSAddonResult::Success();

#if WITH_EDITORONLY_DATA
	// A spatially loaded manager would be moved into a runtime cell and fail
	// ActivateAsWorldManager, which requires the persistent level.
	bIsSpatiallyLoaded = false;
#endif

	SetRootComponent(
		CreateDefaultSubobject<USceneComponent>(TEXT("ManagerRoot")));

#if WITH_EDITORONLY_DATA
	ManagerSprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(
		TEXT("ManagerSprite"));
	if (ManagerSprite)
	{
		static ConstructorHelpers::FObjectFinder<UTexture2D> DefaultSprite(
			TEXT("/Engine/EditorResources/S_Actor"));
		if (DefaultSprite.Succeeded())
		{
			ManagerSprite->SetSprite(DefaultSprite.Object);
		}

		ManagerSprite->SpriteInfo.Category = TEXT("EasyMultiSave");
		ManagerSprite->SpriteInfo.DisplayName =
			LOCTEXT("EMSSpriteCategory", "Easy Multi Save");
		ManagerSprite->bIsScreenSizeScaled = true;
		ManagerSprite->SetupAttachment(GetRootComponent());
	}
#endif
}

void AEMSInstancedSourceManager::BeginPlay()
{
	Super::BeginPlay();

	// A level that is hidden and shown again routes End Play and then Begin Play
	// on the same actor, so this has to clear or the manager can never capture or
	// restore again for the rest of the session.
	bIsEndingPlay = false;
	if (!ActivateAsWorldManager())
	{
		return;
	}

	RefreshLoadedSources();

	// A load clears this again in ActorPreLoad, so real restoration is unaffected.
	MarkLoadedSourcesRestored();

	StreamingStateChangedHandle =
		FLevelStreamingDelegates::OnLevelStreamingStateChanged.AddUObject(
			this,
			&AEMSInstancedSourceManager::HandleLevelStreamingStateChanged);
	BeginMakingInvisibleHandle =
		FLevelStreamingDelegates::OnLevelBeginMakingInvisible.AddUObject(
			this,
			&AEMSInstancedSourceManager::HandleLevelBeginMakingInvisible);
}

void AEMSInstancedSourceManager::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	bIsEndingPlay = true;
	bStreamingRestoreQueued = false;
	FLevelStreamingDelegates::OnLevelStreamingStateChanged.Remove(
		StreamingStateChangedHandle);
	FLevelStreamingDelegates::OnLevelBeginMakingInvisible.Remove(
		BeginMakingInvisibleHandle);
	InvalidateInstanceKeyCaches();
	bIsActiveManager = false;
	Super::EndPlay(EndPlayReason);
}

bool AEMSInstancedSourceManager::ActivateAsWorldManager()
{
	UWorld* World = GetWorld();
	if (!World || GetLevel() != World->PersistentLevel)
	{
		UE_LOG(
			LogEMSAddonsInstances,
			Error,
			TEXT("EMS instanced source manager must be placed in the persistent level. Manager=%s"),
			*GetPathName());
		return false;
	}

	for (TActorIterator<AEMSInstancedSourceManager> It(World); It; ++It)
	{
		AEMSInstancedSourceManager* Other = *It;
		if (Other != this
			&& Other->bIsActiveManager
			&& (Other->IsA(GetClass()) || IsA(Other->GetClass())))
		{
			UE_LOG(
				LogEMSAddonsInstances,
				Error,
				TEXT("Multiple active EMS instanced source managers of the same kind are not supported. Manager=%s Existing=%s"),
				*GetPathName(),
				*Other->GetPathName());
			return false;
		}
	}

	bIsActiveManager = true;
	return true;
}

FName AEMSInstancedSourceManager::MakeLevelIdentity(ULevel* Level)
{
	if (!Level)
	{
		return NAME_None;
	}

	// A World Partition runtime cell is only a transient loading container. Its
	// /Memory package and even its runtime-cell layout are not source identity.
	// Scope the source to the persistent world instead; the resolved actor-instance
	// GUID identifies the actual placed actor inside that world.
	if (Level->GetWorldPartitionRuntimeCell())
	{
		const UWorld* World = Level->GetWorld();
		if (World && World->PersistentLevel
			&& World->PersistentLevel->GetOutermost())
		{
			const FString WorldIdentity = UWorld::RemovePIEPrefix(
				World->PersistentLevel->GetOutermost()->GetName());
			return FName(*(WorldIdentity + TEXT("|WP")));
		}
		return NAME_None;
	}

	// Conventional streaming levels keep their established identity format so
	// existing non-WP saves remain compatible. Level instances additionally use
	// Unreal's resolved level-instance GUID, exactly as before this WP hardening.
	FString LevelIdentity = Level->GetOutermost()
		? UWorld::RemovePIEPrefix(Level->GetOutermost()->GetName())
		: FString();

	const FGuid LevelInstanceGuid =
		FActorInstanceGuid::GetLevelInstanceGuid(Level);
	if (LevelInstanceGuid.IsValid())
	{
		LevelIdentity += TEXT("|");
		LevelIdentity += LevelInstanceGuid.ToString(EGuidFormats::Digits);
	}

	return FName(*LevelIdentity);
}

bool AEMSInstancedSourceManager::BuildStableOwnerIdentity(
	const AActor* SourceOwner,
	FGuid& OutOwnerInstanceGuid,
	FSoftObjectPath& OutOwnerPath)
{
	OutOwnerInstanceGuid.Invalidate();
	OutOwnerPath.Reset();
	if (!SourceOwner || !FActorHelpers::IsPlacedActor(SourceOwner))
	{
		return false;
	}

	const ULevel* SourceLevel = SourceOwner->GetLevel();
	if (SourceLevel && SourceLevel->GetWorldPartitionRuntimeCell())
	{
		// World Partition object paths are rooted in transient /Memory packages.
		// Unreal's resolved actor-instance GUID is the durable actor identity and
		// already accounts for Level Instance context. If Unreal cannot provide it
		// at this point, skip rather than persist under an approximate identity.
		OutOwnerInstanceGuid =
			FActorInstanceGuid::GetActorInstanceGuid(*SourceOwner);
		return OutOwnerInstanceGuid.IsValid();
	}

	// Preserve the established path identity for ordinary streaming levels. It is
	// stable there and retaining it keeps existing non-WP instance saves valid.
	OutOwnerPath = FSoftObjectPath(SourceOwner);
	return OutOwnerPath.IsValid();
}

void AEMSInstancedSourceManager::RegisterSource(
	const FEMSInstanceSourceId& SourceId,
	UInstancedStaticMeshComponent* Component)
{
	if (!SourceId.IsValid()
		|| !IsValid(Component)
		|| AmbiguousSources.Contains(SourceId))
	{
		return;
	}

	if (const TWeakObjectPtr<UInstancedStaticMeshComponent>* Existing =
		LoadedSources.Find(SourceId);
		Existing && Existing->IsValid() && Existing->Get() != Component)
	{
		SourceIdsByComponent.Remove(FObjectKey(Existing->Get()));
		LoadedSources.Remove(SourceId);
		AmbiguousSources.Add(SourceId);
		SourceBaselines.Remove(SourceId);
		ForgetSource(SourceId);
		LogSourceIssue(
			SourceId,
			LOCTEXT(
				"AmbiguousInstancedSource",
				"More than one loaded source has the same instanced source identity."));
		return;
	}

	LoadedSources.Add(SourceId, Component);
	SourceIdsByComponent.Add(FObjectKey(Component), SourceId);
}

bool AEMSInstancedSourceManager::FindRegisteredSourceId(
	const UInstancedStaticMeshComponent* Component,
	FEMSInstanceSourceId& OutSourceId) const
{
	const FEMSInstanceSourceId* Found = Component
		? SourceIdsByComponent.Find(FObjectKey(Component))
		: nullptr;
	if (!Found)
	{
		return false;
	}

	OutSourceId = *Found;
	return true;
}

bool AEMSInstancedSourceManager::IsStableSourceOwner(const AActor* SourceOwner)
{
	FGuid OwnerInstanceGuid;
	FSoftObjectPath OwnerPath;
	if (BuildStableOwnerIdentity(SourceOwner, OwnerInstanceGuid, OwnerPath))
	{
		return true;
	}

	++UnstableSourceOwners;
	UE_LOG(
		LogEMSAddonsInstances,
		Verbose,
		TEXT("EMS instanced source owner is not level-placed or has no stable persistent identity. Manager=%s Owner=%s"),
		*GetPathName(),
		SourceOwner ? *SourceOwner->GetPathName() : TEXT("None"));
	return false;
}

void AEMSInstancedSourceManager::MarkLoadedSourcesRestored()
{
	RestoredSources.Reset();
	for (const TPair<
		FEMSInstanceSourceId,
		TWeakObjectPtr<UInstancedStaticMeshComponent>>& Pair : LoadedSources)
	{
		RestoredSources.Add(Pair.Key);
	}
}

void AEMSInstancedSourceManager::MarkSourceRestored(
	const FEMSInstanceSourceId& SourceId)
{
	if (SourceId.IsValid())
	{
		RestoredSources.Add(SourceId);
	}
}

void AEMSInstancedSourceManager::ForgetSource(
	const FEMSInstanceSourceId& SourceId)
{
	RestoredSources.Remove(SourceId);
}

AEMSInstancedSourceManager::FResolvedSourceState
AEMSInstancedSourceManager::BuildResolvedSourceState(
	const UInstancedStaticMeshComponent* Component,
	const bool bWorldSpace) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EMSAddons_BuildInstancedBaseline);
	using namespace EMSAddons::Instances;

	FResolvedSourceState Result;
	if (!Component)
	{
		Result.bIsValid = false;
		return Result;
	}

	Result.CustomDataFloatCount = Component->NumCustomDataFloats;
	Result.Instances.Reserve(Component->GetInstanceCount());
	for (int32 Index = 0; Index < Component->GetInstanceCount(); ++Index)
	{
		FTransform Transform;
		// FTransform::IsValid rejects any non-finite component and requires a
		// normalized rotation, so NaN and infinity are both covered.
		if (!Component->GetInstanceTransform(Index, Transform, bWorldSpace)
			|| !Transform.IsValid())
		{
			Result.bIsValid = false;
			return Result;
		}

		FResolvedInstance& Instance = Result.Instances.AddDefaulted_GetRef();
		Instance.Transform = Transform;
		Instance.Canonical = CanonicalizeTransform(Transform);
		Instance.Key.TransformHash = HashCanonicalTransform(Instance.Canonical);
		Instance.RuntimeIndex = Index;

		if (Result.CustomDataFloatCount > 0)
		{
			const int32 DataOffset = Index * Result.CustomDataFloatCount;
			if (!Component->PerInstanceSMCustomData.IsValidIndex(
					DataOffset + Result.CustomDataFloatCount - 1))
			{
				Result.bIsValid = false;
				return Result;
			}

			Instance.CustomData.Append(
				Component->PerInstanceSMCustomData.GetData() + DataOffset,
				Result.CustomDataFloatCount);
			if (!IsValidCustomData(
					Instance.CustomData,
					Result.CustomDataFloatCount))
			{
				Result.bIsValid = false;
				return Result;
			}
		}
	}

	Result.Instances.Sort(
		[](const FResolvedInstance& Left, const FResolvedInstance& Right)
		{
			if (Left.Canonical == Right.Canonical)
			{
				return Left.RuntimeIndex < Right.RuntimeIndex;
			}
			return CanonicalTransformLess(Left.Canonical, Right.Canonical);
		});

	uint16 DuplicateOrdinal = 0;
	for (int32 Index = 0; Index < Result.Instances.Num(); ++Index)
	{
		FResolvedInstance& Instance = Result.Instances[Index];
		if (Index > 0
			&& Result.Instances[Index - 1].Canonical == Instance.Canonical)
		{
			if (DuplicateOrdinal == TNumericLimits<uint16>::Max())
			{
				Result.bOrdinalOverflow = true;
			}
			else
			{
				++DuplicateOrdinal;
			}
		}
		else
		{
			DuplicateOrdinal = 0;
		}
		Instance.Key.DuplicateOrdinal = DuplicateOrdinal;
	}

	Result.BaselineSignature = EMSAddons::Hash::OffsetBasis;
	HashCanonicalInt64(Result.BaselineSignature, Result.Instances.Num());
	HashCanonicalInt64(Result.BaselineSignature, Result.CustomDataFloatCount);
	for (const FResolvedInstance& Instance : Result.Instances)
	{
		HashCanonicalTransformSet(Result.BaselineSignature, Instance.Canonical);
		for (const float Value : Instance.CustomData)
		{
			uint32 Bits = 0;
			FMemory::Memcpy(&Bits, &Value, sizeof(float));
			HashCanonicalInt64(Result.BaselineSignature, Bits);
		}
	}
	return Result;
}

void AEMSInstancedSourceManager::AttachAndPruneGameplayData(
	const FEMSInstanceSourceId& SourceId,
	FResolvedSourceState& InOutState)
{
	TMap<FEMSInstanceKey, FEMSInstanceGameplayData>* SourceGameplayData =
		LiveGameplayData.Find(SourceId);
	if (!SourceGameplayData)
	{
		return;
	}

	TSet<FEMSInstanceKey> ResolvedKeys;
	ResolvedKeys.Reserve(InOutState.Instances.Num());
	for (const FResolvedInstance& Instance : InOutState.Instances)
	{
		ResolvedKeys.Add(Instance.Key);
	}

	for (auto It = SourceGameplayData->CreateIterator(); It; ++It)
	{
		if (!ResolvedKeys.Contains(It.Key())
			|| It.Value().IsEmpty()
			|| !IsValidGameplayData(It.Value()))
		{
			It.RemoveCurrent();
		}
	}

	for (FResolvedInstance& Instance : InOutState.Instances)
	{
		if (const FEMSInstanceGameplayData* GameplayData =
			SourceGameplayData->Find(Instance.Key))
		{
			Instance.GameplayData = *GameplayData;
		}
	}

	if (SourceGameplayData->IsEmpty())
	{
		LiveGameplayData.Remove(SourceId);
	}
}

void AEMSInstancedSourceManager::RefreshLoadedSources()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EMSAddons_RefreshLoadedSources);
	if (!GetWorld())
	{
		return;
	}

	LastSourceRefreshFrame = GFrameCounter;

	LoadedSources.Reset();
	AmbiguousSources.Reset();
	SourceIdsByComponent.Reset();
	InvalidateInstanceKeyCaches();

	UnstableSourceOwners = 0;
	CollectSources();
	if (UnstableSourceOwners > 0)
	{
		UE_LOG(
			LogEMSAddonsInstances,
			Warning,
			TEXT("%d instanced source owners were skipped because they are not level-placed or have no stable persistent identity. Manager=%s"),
			UnstableSourceOwners,
			*GetPathName());
	}

	TSet<FEMSInstanceSourceId> ChangedSources;
	ChangedSources.Reserve(SavedDeltas.Num());
	for (const FEMSInstanceSourceDelta& Delta : SavedDeltas)
	{
		ChangedSources.Add(Delta.SourceId);
	}

	for (auto It = SourceBaselines.CreateIterator(); It; ++It)
	{
		if (AmbiguousSources.Contains(It.Key())
			|| (!LoadedSources.Contains(It.Key())
				&& !ChangedSources.Contains(It.Key())))
		{
			It.RemoveCurrent();
		}
	}
	for (auto It = RestoredSources.CreateIterator(); It; ++It)
	{
		const TWeakObjectPtr<UInstancedStaticMeshComponent>* Loaded =
			LoadedSources.Find(*It);
		if (!Loaded || !Loaded->IsValid())
		{
			It.RemoveCurrent();
		}
	}

	for (const TPair<
		FEMSInstanceSourceId,
		TWeakObjectPtr<UInstancedStaticMeshComponent>>& Pair : LoadedSources)
	{
		// The authored baseline is read once, when the source first appears. A
		// changed source keeps it while streamed out because World Partition may
		// only hide the same runtime component rather than reload authored content.
		// Unloaded unchanged sources are pruned above because recapturing their
		// authored state is safe.
		//
		// It must never be re-read merely because the component object changed. A
		// foliage component is replaced whenever its instance set changes, so a
		// restore that empties or fills one would otherwise recapture the restored
		// state as the authored baseline. Every later delta then fails validation
		// against it, and the next capture sees no difference from that corrupted
		// baseline, writes an empty delta, and drops the saved state for good.
		if (!SourceBaselines.Contains(Pair.Key))
		{
			SourceBaselines.Add(
				Pair.Key,
				BuildResolvedSourceState(
					Pair.Value.Get(),
					UsesWorldSpaceTransforms(Pair.Key)));
		}
	}
}

FEMSInstanceSourceDelta AEMSInstancedSourceManager::BuildDelta(
	const FEMSInstanceSourceId& SourceId,
	const FResolvedSourceState& Baseline,
	const FResolvedSourceState& Current) const
{
	using namespace EMSAddons::Instances;

	FEMSInstanceSourceDelta Delta;
	Delta.SourceId = SourceId;
	Delta.BaselineSignature = Baseline.BaselineSignature;
	Delta.BaselineCount = Baseline.Instances.Num();
	Delta.CustomDataFloatCount = Baseline.CustomDataFloatCount;
	int32 BaselineIndex = 0;
	int32 CurrentIndex = 0;
	while (BaselineIndex < Baseline.Instances.Num()
		&& CurrentIndex < Current.Instances.Num())
	{
		const FResolvedInstance& BaselineInstance =
			Baseline.Instances[BaselineIndex];
		const FResolvedInstance& CurrentInstance =
			Current.Instances[CurrentIndex];
		if (BaselineInstance.Canonical == CurrentInstance.Canonical)
		{
			if (!CustomDataEqual(
				BaselineInstance.CustomData,
				CurrentInstance.CustomData))
			{
				FEMSInstanceCustomDataDelta& Modified =
					Delta.ModifiedCustomData.AddDefaulted_GetRef();
				Modified.InstanceKey = BaselineInstance.Key;
				Modified.CustomData = CurrentInstance.CustomData;
			}
			if (!CurrentInstance.GameplayData.IsEmpty())
			{
				FEMSInstanceGameplayDataDelta& Modified =
					Delta.ModifiedGameplayData.AddDefaulted_GetRef();
				Modified.InstanceKey = BaselineInstance.Key;
				Modified.GameplayData = CurrentInstance.GameplayData;
			}
			++BaselineIndex;
			++CurrentIndex;
		}
		else if (CanonicalTransformLess(
			BaselineInstance.Canonical,
			CurrentInstance.Canonical))
		{
			Delta.RemovedInstances.Add(BaselineInstance.Key);
			++BaselineIndex;
		}
		else
		{
			FEMSInstanceState& Added = Delta.AddedInstances.AddDefaulted_GetRef();
			Added.Transform = FTransform3f(CurrentInstance.Transform);
			Added.CustomData = CurrentInstance.CustomData;
			Added.GameplayData = CurrentInstance.GameplayData;
			++CurrentIndex;
		}
	}

	for (; BaselineIndex < Baseline.Instances.Num(); ++BaselineIndex)
	{
		Delta.RemovedInstances.Add(Baseline.Instances[BaselineIndex].Key);
	}
	for (; CurrentIndex < Current.Instances.Num(); ++CurrentIndex)
	{
		const FResolvedInstance& CurrentInstance = Current.Instances[CurrentIndex];
		FEMSInstanceState& Added = Delta.AddedInstances.AddDefaulted_GetRef();
		Added.Transform = FTransform3f(CurrentInstance.Transform);
		Added.CustomData = CurrentInstance.CustomData;
		Added.GameplayData = CurrentInstance.GameplayData;
	}
	return Delta;
}

bool AEMSInstancedSourceManager::CaptureDeltas(const ULevel* OnlyLevel)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EMSAddons_CaptureInstancedDeltas);
	const bool bReportResult = OnlyLevel == nullptr;
	if (!bIsActiveManager || bIsEndingPlay)
	{
		if (bReportResult)
		{
			SkippedCaptureSources = 0;
			LastCaptureResult = FEMSAddonResult(
				EEMSAddonResultCode::Skipped,
				LOCTEXT(
					"InactiveInstancedCapture",
					"The manager is not the active persistent-world manager."));
		}
		return false;
	}

	// Same rule WriteInstanceGameplayData already applies: persistent instance
	// state is server-authoritative, so a client capturing it locally would
	// diverge from the save the server actually keeps.
	if (!HasInstanceAuthority())
	{
		if (bReportResult)
		{
			SkippedCaptureSources = 0;
			LastCaptureResult = FEMSAddonResult(
				EEMSAddonResultCode::Skipped,
				LOCTEXT(
					"UnauthorizedInstancedCapture",
					"Persistent instance state is server-authoritative and is not captured here."));
		}
		return false;
	}

	// A level-scoped pass is invoked before streaming removal, while its already
	// discovered components are still intact. It therefore needs no rediscovery
	// and touches only what that level owns.
	if (bReportResult)
	{
		RefreshLoadedSources();
	}

	int32 SkippedSources = 0;
	TMap<FEMSInstanceSourceId, FEMSInstanceSourceDelta> MergedDeltas;
	for (const FEMSInstanceSourceDelta& Existing : SavedDeltas)
	{
		if (Existing.SourceId.IsValid())
		{
			MergedDeltas.Add(Existing.SourceId, Existing);
		}
	}

	int32 TotalChanges = 0;
	for (const TPair<
		FEMSInstanceSourceId,
		TWeakObjectPtr<UInstancedStaticMeshComponent>>& Pair : LoadedSources)
	{
		const FResolvedSourceState* Baseline = SourceBaselines.Find(Pair.Key);
		UInstancedStaticMeshComponent* Component = Pair.Value.Get();
		if (OnlyLevel)
		{
			const AActor* SourceOwner = Component ? Component->GetOwner() : nullptr;
			if (!SourceOwner || SourceOwner->GetLevel() != OnlyLevel)
			{
				continue;
			}
		}

		if (!Baseline
			|| !Component
			|| !Baseline->bIsValid
			|| Baseline->bOrdinalOverflow
			|| Baseline->CustomDataFloatCount < 0
			|| Baseline->CustomDataFloatCount > MaxCustomDataFloatsPerInstance
			|| Component->NumCustomDataFloats != Baseline->CustomDataFloatCount)
		{
			++SkippedSources;
			LogSourceIssue(
				Pair.Key,
				LOCTEXT(
					"InvalidInstancedBaseline",
					"The source baseline is unavailable or has invalid instance data."));
			continue;
		}

		FResolvedSourceState Current = BuildResolvedSourceState(
			Component,
			UsesWorldSpaceTransforms(Pair.Key));
		if (!Current.bIsValid || Current.bOrdinalOverflow)
		{
			++SkippedSources;
			LogSourceIssue(
				Pair.Key,
				LOCTEXT(
					"InvalidResolvedInstanceState",
					"The current source state contains invalid instance data."));
			continue;
		}
		AttachAndPruneGameplayData(Pair.Key, Current);
		FEMSInstanceSourceDelta Delta = BuildDelta(Pair.Key, *Baseline, Current);
		const int32 SourceChanges =
			Delta.RemovedInstances.Num()
			+ Delta.AddedInstances.Num()
			+ Delta.ModifiedCustomData.Num()
			+ Delta.ModifiedGameplayData.Num();
		if (TotalChanges > MaxTotalChangedInstances - SourceChanges)
		{
			LogSourceIssue(
				Pair.Key,
				LOCTEXT(
					"InstancedDeltaTooLarge",
					"The source delta exceeds the configured safety limit."));
			if (bReportResult)
			{
				SkippedCaptureSources = SkippedSources + 1;
				LastCaptureResult = FEMSAddonResult(
					EEMSAddonResultCode::Skipped,
					LOCTEXT(
						"InstancedCaptureAborted",
						"Capture stopped at the total change limit. Every source kept its previous delta."));
			}
			return false;
		}
		TotalChanges += SourceChanges;

		if (Delta.IsEmpty())
		{
			MergedDeltas.Remove(Pair.Key);
		}
		else
		{
			MergedDeltas.Add(Pair.Key, MoveTemp(Delta));
		}
	}

	if (MergedDeltas.Num() > MaxSourceDeltas)
	{
		UE_LOG(
			LogEMSAddonsInstances,
			Warning,
			TEXT("EMS instanced capture skipped because the source count exceeds the safety limit. Manager=%s Sources=%d"),
			*GetPathName(),
			MergedDeltas.Num());
		if (bReportResult)
		{
			SkippedCaptureSources = SkippedSources + 1;
			LastCaptureResult = FEMSAddonResult(
				EEMSAddonResultCode::Skipped,
				LOCTEXT(
					"InstancedCaptureSourceLimit",
					"Capture was refused at the source limit. Every source kept its previous delta."));
		}
		return false;
	}

	TArray<FEMSInstanceSourceDelta> NewDeltas;
	MergedDeltas.GenerateValueArray(NewDeltas);
	NewDeltas.Sort(
		[](const FEMSInstanceSourceDelta& Left,
			const FEMSInstanceSourceDelta& Right)
		{
			return Left.SourceId.ToString() < Right.SourceId.ToString();
		});
	SavedDeltas = MoveTemp(NewDeltas);

	// One summary line in addition to the per-source warnings, because a partial
	// capture is otherwise only visible by noticing individual entries in a log
	// that a busy save can bury.
	if (SkippedSources > 0)
	{
		UE_LOG(
			LogEMSAddonsInstances,
			Warning,
			TEXT("EMS instanced capture completed with skipped sources. Each kept its previous delta. Manager=%s Skipped=%d Captured=%d"),
			*GetPathName(),
			SkippedSources,
			SavedDeltas.Num());
	}

	if (bReportResult)
	{
		SkippedCaptureSources = SkippedSources;
		LastCaptureResult = SkippedSources > 0
			? FEMSAddonResult(
				EEMSAddonResultCode::Skipped,
				LOCTEXT(
					"InstancedCapturePartial",
					"One or more sources were skipped and kept their previous delta."))
			: FEMSAddonResult::Success();
	}
	return true;
}

FEMSAddonResult AEMSInstancedSourceManager::GetInstancedCaptureResult() const
{
	return LastCaptureResult;
}

int32 AEMSInstancedSourceManager::GetSkippedCaptureSourceCount() const
{
	return SkippedCaptureSources;
}

bool AEMSInstancedSourceManager::ValidateDelta(
	const FEMSInstanceSourceDelta& Delta,
	const FResolvedSourceState& Baseline,
	int32& InOutTotalChanges) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EMSAddons_ValidateInstancedDelta);
	const int32 SourceChanges =
		Delta.RemovedInstances.Num()
		+ Delta.AddedInstances.Num()
		+ Delta.ModifiedCustomData.Num()
		+ Delta.ModifiedGameplayData.Num();
	if (Delta.Version != FEMSInstanceSourceDelta::CurrentVersion
		|| !Delta.SourceId.IsValid()
		|| !Baseline.bIsValid
		|| Delta.BaselineCount != Baseline.Instances.Num()
		|| Delta.BaselineSignature != Baseline.BaselineSignature
		|| Delta.CustomDataFloatCount != Baseline.CustomDataFloatCount
		|| Delta.CustomDataFloatCount < 0
		|| Delta.CustomDataFloatCount > MaxCustomDataFloatsPerInstance
		|| Baseline.bOrdinalOverflow
		|| InOutTotalChanges > MaxTotalChangedInstances - SourceChanges)
	{
		return false;
	}

	TSet<FEMSInstanceKey> BaselineKeys;
	for (const FResolvedInstance& Instance : Baseline.Instances)
	{
		BaselineKeys.Add(Instance.Key);
	}

	TSet<FEMSInstanceKey> RemovedKeys;
	for (const FEMSInstanceKey& Removed : Delta.RemovedInstances)
	{
		if (!BaselineKeys.Contains(Removed) || RemovedKeys.Contains(Removed))
		{
			return false;
		}
		RemovedKeys.Add(Removed);
	}

	TSet<FEMSInstanceKey> ModifiedKeys;
	for (const FEMSInstanceCustomDataDelta& Modified : Delta.ModifiedCustomData)
	{
		if (!BaselineKeys.Contains(Modified.InstanceKey)
			|| RemovedKeys.Contains(Modified.InstanceKey)
			|| ModifiedKeys.Contains(Modified.InstanceKey)
			|| !IsValidCustomData(
				Modified.CustomData,
				Delta.CustomDataFloatCount))
		{
			return false;
		}
		ModifiedKeys.Add(Modified.InstanceKey);
	}

	TSet<FEMSInstanceKey> GameplayKeys;
	for (const FEMSInstanceGameplayDataDelta& Modified : Delta.ModifiedGameplayData)
	{
		if (!BaselineKeys.Contains(Modified.InstanceKey)
			|| RemovedKeys.Contains(Modified.InstanceKey)
			|| GameplayKeys.Contains(Modified.InstanceKey)
			|| Modified.GameplayData.IsEmpty()
			|| !IsValidGameplayData(Modified.GameplayData))
		{
			return false;
		}
		GameplayKeys.Add(Modified.InstanceKey);
	}

	for (const FEMSInstanceState& Added : Delta.AddedInstances)
	{
		if (!FTransform(Added.Transform).IsValid()
			|| !IsValidCustomData(
				Added.CustomData,
				Delta.CustomDataFloatCount)
			|| !IsValidGameplayData(Added.GameplayData))
		{
			return false;
		}
	}

	InOutTotalChanges += SourceChanges;
	return true;
}

AEMSInstancedSourceManager::FResolvedSourceState
AEMSInstancedSourceManager::BuildTargetState(
	const FResolvedSourceState& Baseline,
	const FEMSInstanceSourceDelta* Delta) const
{
	using namespace EMSAddons::Instances;

	FResolvedSourceState TargetState;
	TargetState.BaselineSignature = Baseline.BaselineSignature;
	TargetState.CustomDataFloatCount = Baseline.CustomDataFloatCount;
	TSet<FEMSInstanceKey> RemovedKeys;
	TMap<FEMSInstanceKey, const TArray<float>*> ModifiedData;
	TMap<FEMSInstanceKey, const FEMSInstanceGameplayData*> ModifiedGameplayData;
	if (Delta)
	{
		for (const FEMSInstanceKey& RemovedKey : Delta->RemovedInstances)
		{
			RemovedKeys.Add(RemovedKey);
		}
		for (const FEMSInstanceCustomDataDelta& Modified :
			Delta->ModifiedCustomData)
		{
			ModifiedData.Add(Modified.InstanceKey, &Modified.CustomData);
		}
		for (const FEMSInstanceGameplayDataDelta& Modified :
			Delta->ModifiedGameplayData)
		{
			ModifiedGameplayData.Add(Modified.InstanceKey, &Modified.GameplayData);
		}
	}

	TargetState.Instances.Reserve(
		Baseline.Instances.Num()
		+ (Delta ? Delta->AddedInstances.Num() : 0));
	for (const FResolvedInstance& BaselineInstance : Baseline.Instances)
	{
		if (!RemovedKeys.Contains(BaselineInstance.Key))
		{
			FResolvedInstance& Target =
				TargetState.Instances.Add_GetRef(BaselineInstance);
			if (const TArray<float>* const* CustomData =
				ModifiedData.Find(BaselineInstance.Key))
			{
				Target.CustomData = **CustomData;
			}
			if (const FEMSInstanceGameplayData* const* GameplayData =
				ModifiedGameplayData.Find(BaselineInstance.Key))
			{
				Target.GameplayData = **GameplayData;
			}
		}
	}

	if (Delta)
	{
		for (const FEMSInstanceState& AddedState : Delta->AddedInstances)
		{
			FResolvedInstance& Added =
				TargetState.Instances.AddDefaulted_GetRef();
			Added.Transform = FTransform(AddedState.Transform);
			Added.Canonical = CanonicalizeTransform(Added.Transform);
			Added.CustomData = AddedState.CustomData;
			Added.GameplayData = AddedState.GameplayData;
		}
	}

	TargetState.Instances.StableSort(
		[](const FResolvedInstance& Left, const FResolvedInstance& Right)
		{
			return CanonicalTransformLess(Left.Canonical, Right.Canonical);
		});

	uint16 DuplicateOrdinal = 0;
	for (int32 Index = 0; Index < TargetState.Instances.Num(); ++Index)
	{
		FResolvedInstance& Instance = TargetState.Instances[Index];
		Instance.RuntimeIndex = INDEX_NONE;
		Instance.Key.TransformHash = HashCanonicalTransform(Instance.Canonical);
		if (Index > 0
			&& TargetState.Instances[Index - 1].Canonical == Instance.Canonical)
		{
			if (DuplicateOrdinal == TNumericLimits<uint16>::Max())
			{
				TargetState.bOrdinalOverflow = true;
			}
			else
			{
				++DuplicateOrdinal;
			}
		}
		else
		{
			DuplicateOrdinal = 0;
		}
		Instance.Key.DuplicateOrdinal = DuplicateOrdinal;
	}
	return TargetState;
}

bool AEMSInstancedSourceManager::ReconcileSource(
	const FEMSInstanceSourceId& SourceId,
	const FEMSInstanceSourceDelta* Delta,
	UInstancedStaticMeshComponent* Component,
	const FResolvedSourceState& Baseline)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EMSAddons_ReconcileInstancedSource);
	using namespace EMSAddons::Instances;

	if (!Component
		|| !Baseline.bIsValid
		|| Component->NumCustomDataFloats != Baseline.CustomDataFloatCount)
	{
		return false;
	}

	const FResolvedSourceState Target = BuildTargetState(Baseline, Delta);
	if (Target.bOrdinalOverflow)
	{
		return false;
	}

	const bool bWorldSpace = UsesWorldSpaceTransforms(SourceId);
	FResolvedSourceState Current = BuildResolvedSourceState(Component, bWorldSpace);
	if (!Current.bIsValid || Current.bOrdinalOverflow)
	{
		return false;
	}

	TArray<int32> IndicesToRemove;
	TArray<FTransform> TransformsToAdd;
	int32 CurrentIndex = 0;
	int32 TargetIndex = 0;
	while (CurrentIndex < Current.Instances.Num()
		&& TargetIndex < Target.Instances.Num())
	{
		const FResolvedInstance& CurrentInstance =
			Current.Instances[CurrentIndex];
		const FResolvedInstance& TargetInstance =
			Target.Instances[TargetIndex];
		if (CurrentInstance.Canonical == TargetInstance.Canonical)
		{
			++CurrentIndex;
			++TargetIndex;
		}
		else if (CanonicalTransformLess(
			CurrentInstance.Canonical,
			TargetInstance.Canonical))
		{
			IndicesToRemove.Add(CurrentInstance.RuntimeIndex);
			++CurrentIndex;
		}
		else
		{
			TransformsToAdd.Add(TargetInstance.Transform);
			++TargetIndex;
		}
	}
	for (; CurrentIndex < Current.Instances.Num(); ++CurrentIndex)
	{
		IndicesToRemove.Add(Current.Instances[CurrentIndex].RuntimeIndex);
	}
	for (; TargetIndex < Target.Instances.Num(); ++TargetIndex)
	{
		TransformsToAdd.Add(Target.Instances[TargetIndex].Transform);
	}

	const bool bHasInstanceMutations =
		!IndicesToRemove.IsEmpty() || !TransformsToAdd.IsEmpty();
	if (bHasInstanceMutations)
	{
		IndicesToRemove.Sort(TGreater<int32>());
		InvalidateInstanceKeyCaches();
		if (!ApplyInstanceMutations(
				Component,
				IndicesToRemove,
				TransformsToAdd,
				bWorldSpace))
		{
			return false;
		}
	}

	FResolvedSourceState Verified = bHasInstanceMutations
		? BuildResolvedSourceState(Component, bWorldSpace)
		: MoveTemp(Current);
	if (!Verified.bIsValid || Verified.bOrdinalOverflow)
	{
		return false;
	}
	// Verification asks whether the component accepted the mutations, not whether
	// it stored them bit-for-bit. An instanced component keeps transforms as a
	// float matrix, so writing a saved transform back and reading it returns a
	// value that differs in the last bits, and the quaternion is re-derived from
	// the matrix rather than round-tripped. Canonical rotation is quantized at
	// 1e-6, which that error can straddle: the transform is unchanged to well
	// under a thousandth of a unit while its key lands one step away. Comparing
	// keys alone therefore reported a perfectly restored source as skipped.
	// The transform comparison is the authority; the key check is kept because it
	// is exact whenever the write was in fact lossless.
	bool bMatchesTarget = Verified.Instances.Num() == Target.Instances.Num();
	for (int32 Index = 0;
		bMatchesTarget && Index < Target.Instances.Num();
		++Index)
	{
		const FResolvedInstance& VerifiedInstance = Verified.Instances[Index];
		const FResolvedInstance& TargetInstance = Target.Instances[Index];
		bMatchesTarget = VerifiedInstance.Canonical == TargetInstance.Canonical
			|| IsSameStoredInstance(
				VerifiedInstance.Transform,
				TargetInstance.Transform);
	}
	if (!bMatchesTarget)
	{
		// The counts and the first differing key are logged because this failure
		// is otherwise indistinguishable between its two causes: the component
		// rejected a mutation, or it accepted one and read back a transform that
		// canonicalizes differently. Space is quoted so a local/world mix-up is
		// visible at a glance.
		FString Detail = FString::Printf(
			TEXT(" Space=%s CurrentNum=%d TargetNum=%d"),
			bWorldSpace ? TEXT("World") : TEXT("Local"),
			Verified.Instances.Num(),
			Target.Instances.Num());
		for (int32 Index = 0; Index < Target.Instances.Num(); ++Index)
		{
			if (!Verified.Instances.IsValidIndex(Index))
			{
				break;
			}
			if (!(Verified.Instances[Index].Canonical
				== Target.Instances[Index].Canonical))
			{
				Detail += FString::Printf(
					TEXT(" FirstMismatchAt=%d Got=%s Wanted=%s"),
					Index,
					*Verified.Instances[Index].Transform.ToString(),
					*Target.Instances[Index].Transform.ToString());
				break;
			}
		}

		LogSourceIssue(
			SourceId,
			FText::Format(
				LOCTEXT(
					"InstancedTargetVerificationFailed",
					"The source did not match the saved target after reconciliation.{0}"),
				FText::FromString(Detail)));
		return false;
	}

	bool bCustomDataChanged = false;
	for (int32 Index = 0; Index < Target.Instances.Num(); ++Index)
	{
		const FResolvedInstance& TargetInstance = Target.Instances[Index];
		const FResolvedInstance& VerifiedInstance = Verified.Instances[Index];
		for (int32 DataIndex = 0;
			DataIndex < TargetInstance.CustomData.Num();
			++DataIndex)
		{
			if (!FMath::IsNearlyEqual(
				VerifiedInstance.CustomData[DataIndex],
				TargetInstance.CustomData[DataIndex]))
			{
				Component->SetCustomDataValue(
					VerifiedInstance.RuntimeIndex,
					DataIndex,
					TargetInstance.CustomData[DataIndex],
					false);
				bCustomDataChanged = true;
			}
		}
	}

	// The verified component layout defines the current key space. Saved
	// gameplay state is re-anchored onto those exact resolved keys in one place.
	RebuildGameplayData(SourceId, Target, Verified);
	if (!bCustomDataChanged)
	{
		return true;
	}

	Component->MarkRenderStateDirty();
	Verified = BuildResolvedSourceState(Component, bWorldSpace);
	if (!Verified.bIsValid || Verified.bOrdinalOverflow)
	{
		return false;
	}
	bool bMatchesCustomData =
		Verified.Instances.Num() == Target.Instances.Num();
	for (int32 Index = 0;
		bMatchesCustomData && Index < Target.Instances.Num();
		++Index)
	{
		bMatchesCustomData = CustomDataEqual(
			Verified.Instances[Index].CustomData,
			Target.Instances[Index].CustomData);
	}
	if (!bMatchesCustomData)
	{
		LogSourceIssue(
			SourceId,
			LOCTEXT(
				"InstancedCustomDataVerificationFailed",
				"The source custom data did not match the saved target."));
		return false;
	}
	return true;
}

bool AEMSInstancedSourceManager::ApplyInstanceMutations(
	UInstancedStaticMeshComponent* Component,
	const TArray<int32>& InstanceIndicesToRemove,
	const TArray<FTransform>& TransformsToAdd,
	const bool bWorldSpace) const
{
	// Covers the HISM tree rebuild the component performs for these mutations.
	TRACE_CPUPROFILER_EVENT_SCOPE(EMSAddons_ApplyInstanceMutations);
	if (!InstanceIndicesToRemove.IsEmpty()
		&& !Component->RemoveInstances(InstanceIndicesToRemove, true))
	{
		return false;
	}

	if (!TransformsToAdd.IsEmpty())
	{
		Component->AddInstances(
			TransformsToAdd,
			false,
			bWorldSpace,
			true);
	}

	if (UHierarchicalInstancedStaticMeshComponent* HISM =
		Cast<UHierarchicalInstancedStaticMeshComponent>(Component))
	{
		HISM->BuildTreeIfOutdated(false, true);
	}
	return true;
}

void AEMSInstancedSourceManager::RebuildGameplayData(
	const FEMSInstanceSourceId& SourceId,
	const FResolvedSourceState& Target,
	const FResolvedSourceState& Verified)
{
	LiveGameplayData.Remove(SourceId);
	if (Target.Instances.Num() != Verified.Instances.Num())
	{
		return;
	}

	TMap<FEMSInstanceKey, FEMSInstanceGameplayData> RestoredGameplayData;
	for (int32 Index = 0; Index < Target.Instances.Num(); ++Index)
	{
		const FEMSInstanceGameplayData& GameplayData =
			Target.Instances[Index].GameplayData;
		if (!GameplayData.IsEmpty())
		{
			RestoredGameplayData.Add(
				Verified.Instances[Index].Key,
				GameplayData);
		}
	}
	if (!RestoredGameplayData.IsEmpty())
	{
		LiveGameplayData.Add(SourceId, MoveTemp(RestoredGameplayData));
	}
}

void AEMSInstancedSourceManager::RestoreLoadedSources()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EMSAddons_RestoreInstancedSources);
	if (!bIsActiveManager || bIsEndingPlay)
	{
		bRestorePending = false;
		LastRestoreResult = FEMSAddonResult(
			EEMSAddonResultCode::Skipped,
			LOCTEXT(
				"InactiveInstancedManager",
				"The manager is not the active persistent-world manager."));
		return;
	}

	// Server-authoritative, as in CaptureDeltas. Restoring here would apply saved
	// instance state locally on a client and diverge from the server.
	if (!HasInstanceAuthority())
	{
		bRestorePending = false;
		LastRestoreResult = FEMSAddonResult(
			EEMSAddonResultCode::Skipped,
			LOCTEXT(
				"UnauthorizedInstancedRestore",
				"Persistent instance state is server-authoritative and is not restored here."));
		return;
	}

	bRestorePending = true;
	RefreshLoadedSources();
	bool bHadSkippedSource = false;
	int32 TotalChanges = 0;

	if (SavedDeltas.Num() > MaxSourceDeltas)
	{
		bHadSkippedSource = true;
	}
	else
	{
		TMap<
			FEMSInstanceSourceId,
			const FEMSInstanceSourceDelta*> DeltasBySource;
		DeltasBySource.Reserve(SavedDeltas.Num());
		for (const FEMSInstanceSourceDelta& Delta : SavedDeltas)
		{
			DeltasBySource.FindOrAdd(Delta.SourceId, &Delta);
		}

		for (const TPair<
			FEMSInstanceSourceId,
			TWeakObjectPtr<UInstancedStaticMeshComponent>>& Pair : LoadedSources)
		{
			UInstancedStaticMeshComponent* Component = Pair.Value.Get();
			const FResolvedSourceState* Baseline = SourceBaselines.Find(Pair.Key);
			if (!Component || !Baseline)
			{
				continue;
			}

			if (RestoredSources.Contains(Pair.Key))
			{
				continue;
			}

			const FEMSInstanceSourceDelta* const* SavedDelta =
				DeltasBySource.Find(Pair.Key);
			const FEMSInstanceSourceDelta* Delta =
				SavedDelta ? *SavedDelta : nullptr;
			if (Delta && !ValidateDelta(*Delta, *Baseline, TotalChanges))
			{
				// Which clause rejected the delta is the whole diagnosis here, and
				// it is not recoverable from the identity alone. A saved baseline
				// that no longer matches the live one is the common cause: the
				// source was rediscovered while it already held restored instances,
				// so its authored baseline was recaptured as non-empty.
				FString Detail = FString::Printf(
					TEXT(" SavedBaselineCount=%d LiveBaselineCount=%d"
						" SignatureMatch=%d SavedCustomFloats=%d LiveCustomFloats=%d"
						" Version=%d/%d Removed=%d Added=%d"),
					Delta->BaselineCount,
					Baseline->Instances.Num(),
					Delta->BaselineSignature == Baseline->BaselineSignature ? 1 : 0,
					Delta->CustomDataFloatCount,
					Baseline->CustomDataFloatCount,
					Delta->Version,
					FEMSInstanceSourceDelta::CurrentVersion,
					Delta->RemovedInstances.Num(),
					Delta->AddedInstances.Num());

				bHadSkippedSource = true;
				LogSourceIssue(
					Pair.Key,
					FText::Format(
						LOCTEXT(
							"InvalidSavedInstancedDelta",
							"The saved delta is incompatible or invalid.{0}"),
						FText::FromString(Detail)));
				continue;
			}

			if (ReconcileSource(Pair.Key, Delta, Component, *Baseline))
			{
				RestoredSources.Add(Pair.Key);
			}
			else
			{
				bHadSkippedSource = true;
			}
		}
	}

	bRestorePending = false;

	// A level streaming in re-enters this with nothing left to reconcile, so the
	// skip has to survive until the next load or a single streaming event would
	// report the load as clean after it had skipped a source.
	bHadSkippedRestoreSource |= bHadSkippedSource;
	LastRestoreResult = bHadSkippedRestoreSource
		? FEMSAddonResult(
			EEMSAddonResultCode::Skipped,
			LOCTEXT(
				"InstancedSourcesSkipped",
				"One or more incompatible sources were skipped safely."))
		: FEMSAddonResult::Success();
}

void AEMSInstancedSourceManager::RemoveSourcesInLevel(ULevel* Level)
{
	if (!Level)
	{
		return;
	}

	for (auto It = LoadedSources.CreateIterator(); It; ++It)
	{
		const UInstancedStaticMeshComponent* Component = It.Value().Get();
		const AActor* SourceOwner = Component ? Component->GetOwner() : nullptr;
		if (!SourceOwner || SourceOwner->GetLevel() == Level)
		{
			if (Component)
			{
				SourceIdsByComponent.Remove(FObjectKey(Component));
			}
			ForgetSource(It.Key());
			AmbiguousSources.Remove(It.Key());
			It.RemoveCurrent();
		}
	}
}

void AEMSInstancedSourceManager::HandleLevelStreamingStateChanged(
	UWorld* World,
	const ULevelStreaming* StreamingLevel,
	ULevel* Level,
	ELevelStreamingState PreviousState,
	ELevelStreamingState NewState)
{
	if (World != GetWorld() || bIsEndingPlay || !Level || bHandlingStreamingEvent)
	{
		return;
	}

	// LoadedVisible is the single arrival signal for both ordinary streaming and
	// World Partition. Discovery is queued below because the delegate itself is
	// raised from inside Unreal's final visibility transition.
	if (NewState == ELevelStreamingState::LoadedVisible)
	{
		QueueStreamingRestore();
		return;
	}

	// Sources are dropped only once the level has actually left the world, never
	// on MakingInvisible: that transition is broadcast alongside the
	// OnLevelBeginMakingInvisible capture, and forgetting a source first would
	// race it and lose the delta the capture exists to take.
	if (NewState == ELevelStreamingState::LoadedNotVisible
		|| NewState == ELevelStreamingState::Unloaded
		|| NewState == ELevelStreamingState::Removed
		|| NewState == ELevelStreamingState::FailedToLoad)
	{
		RemoveSourcesInLevel(Level);
	}
}

void AEMSInstancedSourceManager::QueueStreamingRestore()
{
	UWorld* World = GetWorld();
	if (!World || bStreamingRestoreQueued)
	{
		return;
	}

	// The state delegate is raised from inside Unreal's visibility transition.
	// Defer discovery until that transition has returned so level visibility,
	// actors, foliage infos, and component registration all describe one state.
	bStreamingRestoreQueued = true;
	World->GetTimerManager().SetTimerForNextTick(
		FTimerDelegate::CreateWeakLambda(
			this,
			[this]()
			{
				bStreamingRestoreQueued = false;
				if (bIsEndingPlay || bHandlingStreamingEvent)
				{
					return;
				}

				bHandlingStreamingEvent = true;
				RestoreLoadedSources();
				bHandlingStreamingEvent = false;
			}));
}

void AEMSInstancedSourceManager::HandleLevelBeginMakingInvisible(
	UWorld* World,
	const ULevelStreaming* StreamingLevel,
	ULevel* Level)
{
	if (World != GetWorld() || bIsEndingPlay || !Level || bHandlingStreamingEvent)
	{
		return;
	}

	// Capture before Unreal begins unregistering and clearing components. Anything
	// later is too late to read a World Partition cell's foliage and HISM state.
	bHandlingStreamingEvent = true;
	CaptureDeltas(Level);
	bHandlingStreamingEvent = false;
}

bool AEMSInstancedSourceManager::IsInstancedRestorePending() const
{
	return bRestorePending;
}

FEMSAddonResult AEMSInstancedSourceManager::GetInstancedRestoreResult() const
{
	return LastRestoreResult;
}

void AEMSInstancedSourceManager::ActorPreSave_Implementation()
{
	// A refused capture keeps the previous SavedDeltas on purpose. Replacing good
	// data with a partial pass would be worse than saving the last complete one.
	const TWeakObjectPtr<AEMSInstancedSourceManager> WeakThis(this);
	EMSAddons::RunOnGameThread(
		[WeakThis]()
		{
			AEMSInstancedSourceManager* Manager = WeakThis.Get();
			if (IsValid(Manager))
			{
				Manager->CaptureDeltas();
			}
		});
}

void AEMSInstancedSourceManager::ActorPreLoad_Implementation()
{
	RestoredSources.Reset();
	LiveGameplayData.Reset();
	bHadSkippedRestoreSource = false;
	bRestorePending = true;
	LastRestoreResult = FEMSAddonResult(
		EEMSAddonResultCode::Pending,
		LOCTEXT(
			"InstancedRestorePending",
			"Waiting for EMS instanced source delta data."));
}

void AEMSInstancedSourceManager::ActorLoaded_Implementation()
{
	RestoreLoadedSources();
}

bool AEMSInstancedSourceManager::IsEMSAddonRestoreComplete() const
{
	return !bRestorePending;
}

FEMSAddonResult
AEMSInstancedSourceManager::GetEMSAddonRestoreResult() const
{
	return LastRestoreResult;
}

AEMSInstancedSourceManager*
AEMSInstancedSourceManager::GetInstancedSourceManager(
	const UObject* WorldContextObject,
	TSubclassOf<AEMSInstancedSourceManager> ManagerClass)
{
	UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(
			WorldContextObject,
			EGetWorldErrorMode::ReturnNull)
		: nullptr;
	if (!World)
	{
		return nullptr;
	}

	const TSubclassOf<AEMSInstancedSourceManager> ResolvedClass = ManagerClass
		? ManagerClass
		: TSubclassOf<AEMSInstancedSourceManager>(
			AEMSInstancedSourceManager::StaticClass());
	for (TActorIterator<AEMSInstancedSourceManager> It(World, ResolvedClass);
		It;
		++It)
	{
		AEMSInstancedSourceManager* Manager = *It;
		if (IsValid(Manager) && Manager->bIsActiveManager)
		{
			return Manager;
		}
	}
	return nullptr;
}

void AEMSInstancedSourceManager::InvalidateInstanceKeyCaches()
{
	InstanceKeyCaches.Reset();
}

const AEMSInstancedSourceManager::FInstanceKeyCache*
AEMSInstancedSourceManager::GetInstanceKeyCache(
	const UInstancedStaticMeshComponent* Component,
	const int32 QueriedInstanceIndex,
	const EMSAddons::Instances::FCanonicalTransform& QueriedCanonical,
	const bool bWorldSpace) const
{
	using namespace EMSAddons::Instances;

	const FObjectKey ComponentKey(Component);
	const int32 InstanceCount = Component->GetInstanceCount();
	if (const FInstanceKeyCache* Cached = InstanceKeyCaches.Find(ComponentKey);
		Cached
		&& Cached->InstanceCount == InstanceCount
		&& Cached->CanonicalByRuntimeIndex.IsValidIndex(QueriedInstanceIndex)
		&& Cached->CanonicalByRuntimeIndex[QueriedInstanceIndex] == QueriedCanonical)
	{
		return Cached;
	}

	FInstanceKeyCache Rebuilt;
	Rebuilt.InstanceCount = InstanceCount;
	Rebuilt.CanonicalByRuntimeIndex.SetNum(InstanceCount);
	Rebuilt.KeysByRuntimeIndex.SetNum(InstanceCount);

	// Instances with no readable transform keep a default canonical, which the
	// validity check above can never match, so they are simply never resolved.
	TArray<int32> SortedIndices;
	SortedIndices.Reserve(InstanceCount);
	for (int32 Index = 0; Index < InstanceCount; ++Index)
	{
		FTransform Transform;
		if (!Component->GetInstanceTransform(Index, Transform, bWorldSpace)
			|| !Transform.IsValid())
		{
			continue;
		}

		Rebuilt.CanonicalByRuntimeIndex[Index] = CanonicalizeTransform(Transform);
		SortedIndices.Add(Index);
	}

	// Same ordering as BuildResolvedSourceState, so both produce the same keys.
	SortedIndices.Sort(
		[&Rebuilt](const int32 Left, const int32 Right)
		{
			const FCanonicalTransform& LeftCanonical =
				Rebuilt.CanonicalByRuntimeIndex[Left];
			const FCanonicalTransform& RightCanonical =
				Rebuilt.CanonicalByRuntimeIndex[Right];
			if (LeftCanonical == RightCanonical)
			{
				return Left < Right;
			}
			return CanonicalTransformLess(LeftCanonical, RightCanonical);
		});

	uint16 DuplicateOrdinal = 0;
	for (int32 SortedIndex = 0; SortedIndex < SortedIndices.Num(); ++SortedIndex)
	{
		const int32 RuntimeIndex = SortedIndices[SortedIndex];
		const FCanonicalTransform& Canonical =
			Rebuilt.CanonicalByRuntimeIndex[RuntimeIndex];
		if (SortedIndex > 0
			&& Rebuilt.CanonicalByRuntimeIndex[SortedIndices[SortedIndex - 1]]
				== Canonical)
		{
			if (DuplicateOrdinal == TNumericLimits<uint16>::Max())
			{
				// Matches BuildResolvedSourceState's overflow handling: the source
				// is unusable rather than silently mis-keyed.
				return nullptr;
			}
			++DuplicateOrdinal;
		}
		else
		{
			DuplicateOrdinal = 0;
		}

		Rebuilt.KeysByRuntimeIndex[RuntimeIndex].TransformHash =
			HashCanonicalTransform(Canonical);
		Rebuilt.KeysByRuntimeIndex[RuntimeIndex].DuplicateOrdinal = DuplicateOrdinal;
	}

	if (!Rebuilt.CanonicalByRuntimeIndex.IsValidIndex(QueriedInstanceIndex)
		|| !(Rebuilt.CanonicalByRuntimeIndex[QueriedInstanceIndex]
			== QueriedCanonical))
	{
		return nullptr;
	}

	return &InstanceKeyCaches.Add(ComponentKey, MoveTemp(Rebuilt));
}

bool AEMSInstancedSourceManager::ResolveCurrentInstanceKey(
	const UInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex,
	FEMSInstanceSourceId& OutSourceId,
	FEMSInstanceKey& OutKey) const
{
	OutSourceId = FEMSInstanceSourceId();
	OutKey = FEMSInstanceKey();
	if (!IsValid(Component)
		|| InstanceIndex < 0
		|| InstanceIndex >= Component->GetInstanceCount())
	{
		return false;
	}

	const FEMSInstanceSourceId* SourceId =
		SourceIdsByComponent.Find(FObjectKey(Component));
	if (!SourceId || AmbiguousSources.Contains(*SourceId))
	{
		return false;
	}

	// The identity lookup above already resolved this source, so the space it is
	// keyed in comes from that same record rather than a second lookup.
	const bool bWorldSpace = UsesWorldSpaceTransforms(*SourceId);
	FTransform Transform;
	if (!Component->GetInstanceTransform(InstanceIndex, Transform, bWorldSpace)
		|| !Transform.IsValid())
	{
		return false;
	}

	using namespace EMSAddons::Instances;
	const FCanonicalTransform Canonical = CanonicalizeTransform(Transform);
	const FInstanceKeyCache* Cache =
		GetInstanceKeyCache(Component, InstanceIndex, Canonical, bWorldSpace);
	if (!Cache)
	{
		return false;
	}

	OutSourceId = *SourceId;
	OutKey = Cache->KeysByRuntimeIndex[InstanceIndex];
	return true;
}

bool AEMSInstancedSourceManager::IsValidGameplayData(
	const FEMSInstanceGameplayData& GameplayData) const
{
	if (GameplayData.Values.Num() > MaxGameplayValuesPerInstance)
	{
		return false;
	}

	TSet<FGameplayTag> SeenTags;
	SeenTags.Reserve(GameplayData.Values.Num());
	for (const FEMSInstanceGameplayValue& Entry : GameplayData.Values)
	{
		if (!Entry.Tag.IsValid()
			|| !FMath::IsFinite(Entry.Value)
			|| SeenTags.Contains(Entry.Tag))
		{
			return false;
		}
		SeenTags.Add(Entry.Tag);
	}
	return true;
}

bool AEMSInstancedSourceManager::WriteInstanceGameplayData(
	UInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex,
	TFunctionRef<bool(FEMSInstanceGameplayData&)> Mutator)
{
	if (!CanMutateInstanceState())
	{
		return false;
	}

	FEMSInstanceSourceId SourceId;
	FEMSInstanceKey Key;
	if (!ResolveCurrentInstanceKey(Component, InstanceIndex, SourceId, Key))
	{
		// The component may belong to a level that streamed in since the last
		// discovery pass, so give collection one chance to catch up. At most one
		// recovery pass per frame, because a component the manager never persists
		// fails here every call and would otherwise rediscover the whole world
		// once per Blueprint call.
		if (LastSourceRefreshFrame == GFrameCounter)
		{
			return false;
		}

		RefreshLoadedSources();
		if (!ResolveCurrentInstanceKey(Component, InstanceIndex, SourceId, Key))
		{
			return false;
		}
	}

	TMap<FEMSInstanceKey, FEMSInstanceGameplayData>* SourceGameplayData =
		LiveGameplayData.Find(SourceId);
	FEMSInstanceGameplayData Candidate;
	if (SourceGameplayData)
	{
		if (const FEMSInstanceGameplayData* Existing =
			SourceGameplayData->Find(Key))
		{
			Candidate = *Existing;
		}
	}

	if (!Mutator(Candidate) || !IsValidGameplayData(Candidate))
	{
		return false;
	}

	if (Candidate.IsEmpty())
	{
		if (SourceGameplayData)
		{
			SourceGameplayData->Remove(Key);
			if (SourceGameplayData->IsEmpty())
			{
				LiveGameplayData.Remove(SourceId);
			}
		}
		return true;
	}

	LiveGameplayData.FindOrAdd(SourceId).Add(Key, MoveTemp(Candidate));
	return true;
}

bool AEMSInstancedSourceManager::GetInstanceGameplayData(
	const UInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex,
	FEMSInstanceGameplayData& OutGameplayData) const
{
	OutGameplayData = FEMSInstanceGameplayData();

	FEMSInstanceSourceId SourceId;
	FEMSInstanceKey Key;
	if (!ResolveCurrentInstanceKey(Component, InstanceIndex, SourceId, Key))
	{
		return false;
	}

	const TMap<FEMSInstanceKey, FEMSInstanceGameplayData>* SourceGameplayData =
		LiveGameplayData.Find(SourceId);
	const FEMSInstanceGameplayData* Found =
		SourceGameplayData ? SourceGameplayData->Find(Key) : nullptr;
	if (!Found)
	{
		return false;
	}

	OutGameplayData = *Found;
	return true;
}

bool AEMSInstancedSourceManager::GetInstanceGameplayValue(
	const UInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex,
	FGameplayTag Tag,
	float& OutValue) const
{
	OutValue = 0.0f;

	FEMSInstanceSourceId SourceId;
	FEMSInstanceKey Key;
	if (!Tag.IsValid()
		|| !ResolveCurrentInstanceKey(Component, InstanceIndex, SourceId, Key))
	{
		return false;
	}

	const TMap<FEMSInstanceKey, FEMSInstanceGameplayData>* SourceGameplayData =
		LiveGameplayData.Find(SourceId);
	const FEMSInstanceGameplayData* Found =
		SourceGameplayData ? SourceGameplayData->Find(Key) : nullptr;
	const float* Value = Found ? Found->Find(Tag) : nullptr;
	if (!Value)
	{
		return false;
	}

	OutValue = *Value;
	return true;
}

bool AEMSInstancedSourceManager::SetInstanceGameplayValue(
	UInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex,
	FGameplayTag Tag,
	const float Value)
{
	if (!Tag.IsValid() || !FMath::IsFinite(Value))
	{
		return false;
	}

	return WriteInstanceGameplayData(
		Component,
		InstanceIndex,
		[&Tag, Value](FEMSInstanceGameplayData& GameplayData)
		{
			GameplayData.Set(Tag, Value);
			return true;
		});
}

bool AEMSInstancedSourceManager::SetInstanceGameplayData(
	UInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex,
	const FEMSInstanceGameplayData& GameplayData)
{
	return WriteInstanceGameplayData(
		Component,
		InstanceIndex,
		[&GameplayData](FEMSInstanceGameplayData& Target)
		{
			Target = GameplayData;
			return true;
		});
}

bool AEMSInstancedSourceManager::RemoveInstanceGameplayValue(
	UInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex,
	FGameplayTag Tag)
{
	if (!Tag.IsValid())
	{
		return false;
	}

	return WriteInstanceGameplayData(
		Component,
		InstanceIndex,
		[&Tag](FEMSInstanceGameplayData& GameplayData)
		{
			return GameplayData.Remove(Tag);
		});
}

bool AEMSInstancedSourceManager::ClearInstanceGameplayData(
	UInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex)
{
	return WriteInstanceGameplayData(
		Component,
		InstanceIndex,
		[](FEMSInstanceGameplayData& GameplayData)
		{
			const bool bHadValues = !GameplayData.IsEmpty();
			GameplayData.Values.Reset();
			return bHadValues;
		});
}

ENetMode AEMSInstancedSourceManager::GetInstanceNetMode() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetNetMode() : NM_Standalone;
}

bool AEMSInstancedSourceManager::HasInstanceAuthority() const
{
	return EMSAddons::HasPersistenceAuthority(this, GetInstanceNetMode());
}

void AEMSInstancedSourceManager::LogSourceIssue(
	const FEMSInstanceSourceId& SourceId,
	const FText& Reason) const
{
	UE_LOG(
		LogEMSAddonsInstances,
		Warning,
		TEXT("EMS instanced source skipped. Manager=%s Source=%s Reason=%s"),
		*GetPathName(),
		*SourceId.ToString(),
		*Reason.ToString());
}

#undef LOCTEXT_NAMESPACE
