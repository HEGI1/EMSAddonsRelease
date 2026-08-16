//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSInstancedSourceManager.h"
#include "EMSInstanceManager.generated.h"

class AInstancedFoliageActor;

/**
 * Persists project-owned ISM and HISM instances and painted Static Mesh Foliage.
 *
 * Place exactly one in the persistent level. Both kinds of source are discovered
 * by this one manager: ordinary instanced components are found on their owning
 * actor, while foliage is found through the Instanced Foliage Actor, which keys
 * its instances in world space rather than relative to the owner. The two are
 * only different at discovery — once a source is registered its component is an
 * ordinary HISM that the shared engine mutates directly, and the space it is
 * keyed in travels with its identity.
 */
UCLASS(
	Blueprintable,
	ClassGroup = (EasyMultiSave),
	meta = (DisplayName = "EMS Instance Manager"))
class EMSADDONSINSTANCES_API AEMSInstanceManager
	: public AEMSInstancedSourceManager
{
	GENERATED_BODY()

protected:
	virtual void CollectSources() override;

private:
	bool IsSupportedActor(const AActor* Actor) const;
	bool IsSupportedSource(const UInstancedStaticMeshComponent* Component) const;
	FEMSInstanceSourceId BuildSourceId(
		const UInstancedStaticMeshComponent* Component) const;

	/** Registers every painted Static Mesh Foliage type the actor carries. */
	void CollectFoliageSources(AInstancedFoliageActor* FoliageActor);
};
