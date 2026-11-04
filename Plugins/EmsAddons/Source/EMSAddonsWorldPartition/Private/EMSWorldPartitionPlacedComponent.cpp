//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSWorldPartitionPlacedComponent.h"

#include "EMSAddonsWorldPartition.h"
#include "EMSActorSaveInterface.h"
#include "Engine/Level.h"
#include "WorldPartition/WorldPartition.h"
#include "EMSWorldPartitionPlacedSubsystem.h"
#include "EMSTypes.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/ObjectSaveContext.h"
#if WITH_EDITOR
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/HLOD/HLODActor.h"
#endif

UEMSWorldPartitionPlacedComponent::UEMSWorldPartitionPlacedComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bWantsInitializeComponent = true;
}

void UEMSWorldPartitionPlacedComponent::PostLoad()
{
	Super::PostLoad();
	bLoadedPlacement = !IsTemplate() && PlacementId.IsValid();
#if WITH_EDITOR
	EnforceNonSpatialPlacement();
#endif
}

void UEMSWorldPartitionPlacedComponent::PostDuplicate(const EDuplicateMode::Type DuplicateMode)
{
	Super::PostDuplicate(DuplicateMode);
	bLoadedPlacement = DuplicateMode == EDuplicateMode::PIE && PlacementId.IsValid();
#if WITH_EDITOR
	if (DuplicateMode != EDuplicateMode::PIE && !IsTemplate() && GetWorld() && !GetWorld()->IsGameWorld())
	{
		PlacementId = FGuid::NewGuid();
		EnforceNonSpatialPlacement();
	}
#endif
}

void UEMSWorldPartitionPlacedComponent::OnRegister()
{
	Super::OnRegister();
#if WITH_EDITOR
	EnforceNonSpatialPlacement();
#endif
	// Ordinary EMS discovery must skip map originals even before BeginPlay.
	if (IsAuthoredPlacement() && GetWorld() && GetWorld()->IsGameWorld() && GetOwner()
		&& !bUnsupportedPlacement && GetWorld()->GetNetMode() != NM_Client
		&& GetWorld()->GetWorldPartition() && GetWorld()->GetWorldPartition()->bEnableStreaming
		&& GetOwner()->GetLevel() == GetWorld()->PersistentLevel
		&& GetOwner()->GetClass()->ImplementsInterface(UEMSActorSaveInterface::StaticClass()))
	{
		TInlineComponentArray<UEMSWorldPartitionPlacedComponent*> Markers(GetOwner());
		if (Markers.Num() == 1)
		{
			bAddedEarlySkipSaveTag |= !GetOwner()->ActorHasTag(EMS::SkipSaveTag);
			GetOwner()->Tags.AddUnique(EMS::SkipSaveTag);
		}
	}
}

void UEMSWorldPartitionPlacedComponent::ReleaseEarlySaveExclusion()
{
	if (bAddedEarlySkipSaveTag && GetOwner()) GetOwner()->Tags.Remove(EMS::SkipSaveTag);
	bAddedEarlySkipSaveTag = false;
}

void UEMSWorldPartitionPlacedComponent::InitializeComponent()
{
	Super::InitializeComponent();
	// Capture authored state after construction but before the owner's BeginPlay
	// can change SaveGame properties. BeginPlay retries if setup was not ready.
	if (IsAuthoredPlacement() && GetWorld())
	{
		if (UEMSWorldPartitionPlacedSubsystem* Subsystem = GetWorld()->GetSubsystem<UEMSWorldPartitionPlacedSubsystem>())
		{
			if (!Subsystem->IsRestoringActor()) Subsystem->RegisterPlacedActor(this);
		}
	}
}

void UEMSWorldPartitionPlacedComponent::BeginPlay()
{
	Super::BeginPlay();
	if (UWorld* World = GetWorld())
	{
		if (IsAuthoredPlacement() && bUnsupportedPlacement && World->GetNetMode() != NM_Client)
		{
			UE_LOG(LogEMSAddonsWorldPartition, Error, TEXT("Rejected placed actor authoring: require a main World Partition placement without runtime Data Layers, Level Instances, sublevels, or HLOD ownership. Actor=%s"), *GetNameSafe(GetOwner()));
			ReleaseEarlySaveExclusion();
			return;
		}
		if (UEMSWorldPartitionPlacedSubsystem* Subsystem = World->GetSubsystem<UEMSWorldPartitionPlacedSubsystem>())
		{
			if (Subsystem->IsRestoringActor()) return;
			if (IsAuthoredPlacement())
			{
				Subsystem->RegisterPlacedActor(this);
			}
			else
			{
				UE_LOG(LogEMSAddonsWorldPartition, Warning,
					TEXT("World Partition Placed Component requires an editor-authored placement; runtime additions and ordinary runtime spawns are ignored. Actor=%s"),
					*GetNameSafe(GetOwner()));
			}
		}
	}
}

#if WITH_EDITOR
void UEMSWorldPartitionPlacedComponent::EnforceNonSpatialPlacement()
{
	AActor* Owner = GetOwner();
	if (IsTemplate() || !Owner || Owner->IsTemplate() || !GetWorld() || GetWorld()->IsGameWorld()) return;
	bool bUnsupported = !GetWorld()->GetWorldPartition() || !GetWorld()->GetWorldPartition()->bEnableStreaming
		|| Owner->GetLevel() != GetWorld()->PersistentLevel || Owner->IsInLevelInstance() || Owner->IsA<AWorldPartitionHLOD>();
	for (const UDataLayerAsset* Layer : Owner->GetDataLayerAssets()) bUnsupported |= Layer && Layer->IsRuntime();
	if (bUnsupportedPlacement != bUnsupported) { bUnsupportedPlacement = bUnsupported; MarkPackageDirty(); }
	const FGuid OwnerId = Owner->GetActorGuid();
	if (OwnerId.IsValid() && PlacementId != OwnerId)
	{
		PlacementId = OwnerId;
		MarkPackageDirty();
	}
	if (Owner->GetIsSpatiallyLoaded())
	{
		Owner->Modify();
		Owner->SetIsSpatiallyLoaded(false);
		Owner->MarkPackageDirty();
	}
}

void UEMSWorldPartitionPlacedComponent::PostEditImport()
{
	Super::PostEditImport();
	if (!IsTemplate() && GetWorld() && !GetWorld()->IsGameWorld()) PlacementId = FGuid::NewGuid();
	EnforceNonSpatialPlacement();
}

void UEMSWorldPartitionPlacedComponent::PreSave(FObjectPreSaveContext SaveContext)
{
	EnforceNonSpatialPlacement();
	Super::PreSave(SaveContext);
}
#endif

FEMSPlacedComponentInstanceData::FEMSPlacedComponentInstanceData(const UEMSWorldPartitionPlacedComponent* Source)
	: FActorComponentInstanceData(Source), PlacementId(Source->PlacementId), bLoadedPlacement(Source->bLoadedPlacement)
{
}

void FEMSPlacedComponentInstanceData::ApplyToComponent(UActorComponent* Component, ECacheApplyPhase Phase)
{
	FActorComponentInstanceData::ApplyToComponent(Component, Phase);
	if (UEMSWorldPartitionPlacedComponent* Marker = Cast<UEMSWorldPartitionPlacedComponent>(Component))
	{
		Marker->PlacementId = PlacementId;
		Marker->bLoadedPlacement = bLoadedPlacement;
#if WITH_EDITOR
		Marker->EnforceNonSpatialPlacement();
#endif
	}
}

TStructOnScope<FActorComponentInstanceData> UEMSWorldPartitionPlacedComponent::GetComponentInstanceData() const
{
	return MakeStructOnScope<FActorComponentInstanceData, FEMSPlacedComponentInstanceData>(this);
}
