//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSActorSaveInterface.h"
#include "EMSWorldPartitionRuntimeTypes.h"
#include "GameFramework/Actor.h"
#include "EMSWorldPartitionRuntimeManager.generated.h"

UCLASS(NotPlaceable, ClassGroup = (EasyMultiSave), meta = (DisplayName = "EMS World Partition Runtime Manager"))
class EMSADDONSWORLDPARTITION_API AEMSWorldPartitionRuntimeManager
	: public AActor, public IEMSActorSaveInterface
{
	GENERATED_BODY()

public:
	AEMSWorldPartitionRuntimeManager();

	UPROPERTY(SaveGame, VisibleInstanceOnly, BlueprintReadOnly, Category = "EMS Addons|World Partition Runtime Actors")
	TArray<FEMSWorldPartitionRuntimeActorRecord> RuntimeActorRecords;

	virtual void ActorPreSave_Implementation() override;
	virtual void ActorPreLoad_Implementation() override;
	virtual void ActorLoaded_Implementation() override;
};
