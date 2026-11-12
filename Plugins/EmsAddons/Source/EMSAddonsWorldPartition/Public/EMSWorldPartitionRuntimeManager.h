//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSActorSaveInterface.h"
#include "EMSWorldPartitionRuntimeTypes.h"
#include "EMSWorldPartitionPlacedTypes.h"
#include "GameFramework/Actor.h"
#include "EMSWorldPartitionRuntimeManager.generated.h"

UCLASS(NotPlaceable, ClassGroup = (EasyMultiSave), meta = (DisplayName = "EMS World Partition Runtime Manager"))
class EMSADDONSWORLDPARTITION_API AEMSWorldPartitionRuntimeManager
	: public AActor, public IEMSActorSaveInterface
{
	GENERATED_BODY()

public:
	AEMSWorldPartitionRuntimeManager();
	virtual void Serialize(FArchive& Ar) override;

	UPROPERTY(SaveGame, VisibleInstanceOnly, BlueprintReadOnly, Category = "EMS Addons|World Partition Runtime Actors")
	TArray<FEMSWorldPartitionRuntimeActorRecord> RuntimeActorRecords;

	UPROPERTY(SaveGame, VisibleInstanceOnly, Category = "EMS Addons|World Partition Placed Actors")
	TArray<FEMSWorldPartitionPlacedActorRecord> PlacedActorRecords;

	virtual void ActorPreSave_Implementation() override;
	virtual void ActorPreLoad_Implementation() override;
	virtual void ActorLoaded_Implementation() override;

private:
	/** PIE-independent map package; old slot-wide records have no safe map ownership. */
	UPROPERTY(SaveGame)
	FName SavedWorldPackage;

	bool bRejectedSaveData = false;
};
