//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAddonsWorldPartition.h"
#if WITH_EDITOR
#include "EMSWorldPartitionPlacedComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/ObjectSaveContext.h"
#endif

DEFINE_LOG_CATEGORY(LogEMSAddonsWorldPartition);

void FEMSAddonsWorldPartitionModule::StartupModule()
{
#if WITH_EDITOR
	PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
		this, &FEMSAddonsWorldPartitionModule::HandleObjectPropertyChanged);
	// UObject::PreSave broadcasts before AActor builds external-package descriptor
	// tags. The marker's own PreSave can run after those tags are generated.
	PreSaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddLambda(
		[this](UObject* Object, FObjectPreSaveContext) { EnforcePlacement(Object); });
	TransactedHandle = FCoreUObjectDelegates::OnObjectTransacted.AddLambda(
		[this](UObject* Object, const FTransactionObjectEvent&) { EnforcePlacement(Object); });
#endif
}

void FEMSAddonsWorldPartitionModule::ShutdownModule()
{
#if WITH_EDITOR
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
	FCoreUObjectDelegates::OnObjectPreSave.Remove(PreSaveHandle);
	FCoreUObjectDelegates::OnObjectTransacted.Remove(TransactedHandle);
#endif
}

#if WITH_EDITOR
void FEMSAddonsWorldPartitionModule::HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	EnforcePlacement(Object);
}

void FEMSAddonsWorldPartitionModule::EnforcePlacement(UObject* Object)
{
	static bool bEnforcing = false;
	if (bEnforcing || !IsValid(Object)) return;
	TGuardValue<bool> Guard(bEnforcing, true);
	if (UEMSWorldPartitionPlacedComponent* Component = Cast<UEMSWorldPartitionPlacedComponent>(Object))
	{
		Component->EnforceNonSpatialPlacement();
	}
	else if (AActor* Actor = Cast<AActor>(Object))
	{
		TInlineComponentArray<UEMSWorldPartitionPlacedComponent*> Components(Actor);
		for (UEMSWorldPartitionPlacedComponent* Marker : Components) Marker->EnforceNonSpatialPlacement();
	}
}
#endif

IMPLEMENT_MODULE(FEMSAddonsWorldPartitionModule, EMSAddonsWorldPartition)
