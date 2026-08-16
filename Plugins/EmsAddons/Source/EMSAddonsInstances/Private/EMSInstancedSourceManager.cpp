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
#include "EngineUtils.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/ConstructorHelpers.h"
#include "WorldPartition/ActorInstanceGuids.h"

#define LOCTEXT_NAMESPACE "EMSInstancedSourceManager"

namespace
{
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

	LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddUObject(
		this,
		&AEMSInstancedSourceManager::HandleLevelAdded);
	LevelRemovedHandle = FWorldDelegates::LevelRemovedFromWorld.AddUObject(
		this,
		&AEMSInstancedSourceManager::HandleLevelRemoved);
}

void AEMSInstancedSourceManager::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	bIsEndingPlay = true;
	FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
	FWorldDelegates::LevelRemovedFromWorld.Remove(LevelRemovedHandle);
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
	// EMS strips the PIE prefix from everything it stores so editor saves stay
	// loadable in a packaged build, and the save archive does it for soft object
	// paths on its own. An FName is not a path to the archive, so this one has to
	// be normalized here or every saved delta is skipped outside the editor.
	FString LevelIdentity = Level && Level->GetOutermost()
		? UWorld::RemovePIEPrefix(Level->GetOutermost()->GetName())
		: FString();

	const FGuid LevelInstanceGuid =
		Level ? FActorInstanceGuid::GetLevelInstanceGuid(Level) : FGuid();
	if (LevelInstanceGuid.IsValid())
	{
		LevelIdentity += TEXT("|");
		LevelIdentity += LevelInstanceGuid.ToString(EGuidFormats::Digits);
	}

	return FName(*LevelIdentity);
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

bool AEMSInstancedSourceManager::IsStableSourceOwner(const AActor* SourceOwner)
{
	if (FActorHelpers::IsPlacedActor(SourceOwner))
	{
		return true;
	}

	++UnstableSourceOwners;
	UE_LOG(
		LogEMSAddonsInstances,
		Verbose,
		TEXT("EMS instanced source owner is not level-placed and is not persisted. Manager=%s Owner=%s"),
		*GetPathName(),
		SourceOwner ? *SourceOwner->GetPathName() : TEXT("None"));
	return false;
}

void AEMSInstancedSourceManager::MarkLoadedSourcesRestored()
{
	RestoredSources = LoadedSources;
}

void AEMSInstancedSourceManager::ForgetSource(
	const FEMSInstanceSourceId& SourceId)
{
	SourceBaselines.Remove(SourceId);
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
			continue;
		}

		FResolvedInstance& Instance = Result.Instances.AddDefaulted_GetRef();
		Instance.Transform = Transform;
		Instance.Canonical = CanonicalizeTransform(Transform);
		Instance.Key.TransformHash = HashCanonicalTransform(Instance.Canonical);
		Instance.RuntimeIndex = Index;

		if (Result.CustomDataFloatCount > 0)
		{
			const int32 DataOffset = Index * Result.CustomDataFloatCount;
			if (Component->PerInstanceSMCustomData.IsValidIndex(
				DataOffset + Result.CustomDataFloatCount - 1))
			{
				Instance.CustomData.Append(
					Component->PerInstanceSMCustomData.GetData() + DataOffset,
					Result.CustomDataFloatCount);
			}
			else
			{
				Instance.CustomData.Init(0.0f, Result.CustomDataFloatCount);
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

	const TMap<
		FEMSInstanceSourceId,
		TWeakObjectPtr<UInstancedStaticMeshComponent>> PreviousSources =
		MoveTemp(LoadedSources);
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
			TEXT("%d instanced source owners were skipped because they are not level-placed and have no identity that survives a save. Manager=%s"),
			UnstableSourceOwners,
			*GetPathName());
	}

	for (auto It = SourceBaselines.CreateIterator(); It; ++It)
	{
		if (!LoadedSources.Contains(It.Key())
			|| AmbiguousSources.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	for (auto It = RestoredSources.CreateIterator(); It; ++It)
	{
		const TWeakObjectPtr<UInstancedStaticMeshComponent>* Loaded =
			LoadedSources.Find(It.Key());
		if (!Loaded || !Loaded->IsValid() || Loaded->Get() != It.Value().Get())
		{
			It.RemoveCurrent();
		}
	}

	for (const TPair<
		FEMSInstanceSourceId,
		TWeakObjectPtr<UInstancedStaticMeshComponent>>& Pair : LoadedSources)
	{
		const TWeakObjectPtr<UInstancedStaticMeshComponent>* Previous =
			PreviousSources.Find(Pair.Key);
		if (!Previous
			|| !Previous->IsValid()
			|| Previous->Get() != Pair.Value.Get()
			|| !SourceBaselines.Contains(Pair.Key))
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

	// A level that is unloading was already discovered, so a level-scoped pass
	// needs no rediscovery and touches only what that level owns.
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
		if (Current.bOrdinalOverflow)
		{
			++SkippedSources;
			LogSourceIssue(
				Pair.Key,
				LOCTEXT(
					"InvalidResolvedInstanceState",
					"The current source state has too many exact duplicate transforms."));
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
	bool bMatchesTarget = Verified.Instances.Num() == Target.Instances.Num();
	for (int32 Index = 0;
		bMatchesTarget && Index < Target.Instances.Num();
		++Index)
	{
		bMatchesTarget =
			Verified.Instances[Index].Canonical
				== Target.Instances[Index].Canonical;
	}
	if (!bMatchesTarget)
	{
		LogSourceIssue(
			SourceId,
			LOCTEXT(
				"InstancedTargetVerificationFailed",
				"The source did not match the saved target after reconciliation."));
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

			if (const TWeakObjectPtr<UInstancedStaticMeshComponent>* Restored =
				RestoredSources.Find(Pair.Key);
				Restored && Restored->Get() == Component)
			{
				continue;
			}

			const FEMSInstanceSourceDelta* const* SavedDelta =
				DeltasBySource.Find(Pair.Key);
			const FEMSInstanceSourceDelta* Delta =
				SavedDelta ? *SavedDelta : nullptr;
			if (Delta && !ValidateDelta(*Delta, *Baseline, TotalChanges))
			{
				bHadSkippedSource = true;
				LogSourceIssue(
					Pair.Key,
					LOCTEXT(
						"InvalidSavedInstancedDelta",
						"The saved delta is incompatible or invalid."));
				continue;
			}

			if (ReconcileSource(Pair.Key, Delta, Component, *Baseline))
			{
				RestoredSources.Add(Pair.Key, Component);
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

void AEMSInstancedSourceManager::HandleLevelAdded(ULevel* Level, UWorld* World)
{
	if (World == GetWorld() && !bIsEndingPlay)
	{
		RestoreLoadedSources();
	}
}

void AEMSInstancedSourceManager::HandleLevelRemoved(
	ULevel* Level,
	UWorld* World)
{
	if (World != GetWorld() || bIsEndingPlay)
	{
		return;
	}

	CaptureDeltas(Level);
	RemoveSourcesInLevel(Level);
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
	EMSAddons::RunOnGameThread([this]() { CaptureDeltas(); });
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
	// Gameplay data is persistent authoritative state, so a client writing it
	// would diverge from the save the server actually keeps.
	if (bIsEndingPlay || !HasInstanceAuthority())
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
