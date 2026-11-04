//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "EMSWorldPartitionPlacedComponent.generated.h"

class UEMSWorldPartitionPlacedSubsystem;
class FObjectPreSaveContext;
struct FEMSPlacedComponentInstanceData;

/** Author on a placed actor in the editor. Runtime addition is unsupported. */
UCLASS(ClassGroup = (EasyMultiSave), meta = (BlueprintSpawnableComponent, DisplayName = "EMS World Partition Placed Component"))
class EMSADDONSWORLDPARTITION_API UEMSWorldPartitionPlacedComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEMSWorldPartitionPlacedComponent();

	UFUNCTION(BlueprintPure, Category = "EMS Addons|World Partition Placed Actors")
	FGuid GetPlacementId() const { return PlacementId; }

	/** Loaded map component or PIE duplicate, never an ordinary runtime addition. */
	bool IsAuthoredPlacement() const { return bLoadedPlacement && PlacementId.IsValid(); }
	virtual void PostLoad() override;
	virtual void PostDuplicate(EDuplicateMode::Type DuplicateMode) override;
	virtual TStructOnScope<FActorComponentInstanceData> GetComponentInstanceData() const override;

#if WITH_EDITOR
	/** Also used by the module's owner-property-change hook. */
	void EnforceNonSpatialPlacement();
	virtual void PostEditImport() override;
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
#endif

protected:
	virtual void OnRegister() override;
	virtual void InitializeComponent() override;
	virtual void BeginPlay() override;

private:
	friend class UEMSWorldPartitionPlacedSubsystem;
	friend struct FEMSPlacedComponentInstanceData;

	/** Serialized with the map, not generated at runtime or stored on the CDO. */
	UPROPERTY(VisibleInstanceOnly, Category = "EMS Addons|World Partition Placed Actors")
	FGuid PlacementId;

	/** Cooked authoring rejection for scopes that can collapse into a persistent level. */
	UPROPERTY()
	bool bUnsupportedPlacement = false;
	bool bAddedEarlySkipSaveTag = false;
	bool bReportedCapacity = false;
	void ReleaseEarlySaveExclusion();
	bool bLoadedPlacement = false;
	void SetRestoredIdentity(const FGuid& Id) { PlacementId = Id; bLoadedPlacement = false; }
};

/** Preserve map provenance as well as identity when construction recreates SCS markers. */
USTRUCT()
struct EMSADDONSWORLDPARTITION_API FEMSPlacedComponentInstanceData : public FActorComponentInstanceData
{
	GENERATED_BODY()
	FEMSPlacedComponentInstanceData() = default;
	explicit FEMSPlacedComponentInstanceData(const UEMSWorldPartitionPlacedComponent* Source);
	virtual bool ContainsData() const override { return true; }
	virtual void ApplyToComponent(UActorComponent* Component, ECacheApplyPhase Phase) override;
	FGuid PlacementId;
	bool bLoadedPlacement = false;
};
