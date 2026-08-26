//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSInstancedSourceManager.h"
#include "EMSInstanceManager.generated.h"

class AInstancedFoliageActor;
class UStaticMesh;

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

public:
	/**
	 * Replaces one managed instance using high-level replacement settings.
	 *
	 * Component and Instance Index can be taken directly from a hit result.
	 * The manager routes the replacement to a compatible instance source,
	 * creating a persistent runtime source when no authored target exists.
	 * Per-instance custom data and EMS gameplay values are carried across
	 * automatically.
	 *
	 * Success means the source instance was actually removed and the replacement
	 * was created. A same-mesh/default no-op is treated as failure rather than as
	 * a successful replacement.
	 *
	 * On success the input Component and Instance Index identify the removed
	 * source instance, not the new replacement. Use Replacement Component and
	 * Replacement Instance Index for any operation that follows the replacement,
	 * such as setting an EMS gameplay value.
	 *
	 * Preserve Scale keeps the original instance scale, including foliage random
	 * scale. Transform Offset is then composed in the original instance's local
	 * space. Disable Preserve Scale to reset the base scale to 1,1,1 before the
	 * offset is applied.
	 *
	 * Authored Foliage Types are never modified. A painted foliage instance is
	 * replaced by a target the manager owns on the Instanced Foliage Actor, so
	 * changing one tree never changes every tree sharing its Foliage Type.
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "EMS Addons|Instanced",
		meta = (
			DisplayName = "Replace Instance",
			ExpandBoolAsExecs = "ReturnValue"))
	bool ReplaceInstanceWithSettings(
		UInstancedStaticMeshComponent* Component,
		int32 InstanceIndex,
		const FEMSInstanceReplacement& Replacement,
		UPARAM(DisplayName = "Replacement Component")
		UInstancedStaticMeshComponent*& OutReplacementComponent,
		UPARAM(DisplayName = "Replacement Instance Index")
		int32& OutReplacementInstanceIndex);

	/** C++ shorthand for the default replacement behavior. Blueprint uses the settings struct. */
	bool ReplaceInstance(
		UInstancedStaticMeshComponent* Component,
		int32 InstanceIndex,
		UStaticMesh* ReplacementMesh);

protected:
	virtual void CollectSources() override;

	/**
	 * Whether a level is assembled enough to read its sources.
	 *
	 * Discovery runs for the whole world whenever any level finishes loading, so
	 * a level that is still streaming in has to be skipped rather than cached as
	 * an authored baseline in its partial state.
	 */
	static bool IsLevelReadyForSourceDiscovery(const ULevel* Level);

private:
	static constexpr int32 MaxReplacementSources = 512;

	UPROPERTY(SaveGame)
	TArray<FEMSInstanceReplacementSource> SavedReplacementSources;

	bool IsSupportedActor(const AActor* Actor) const;
	bool IsSupportedSource(const UInstancedStaticMeshComponent* Component) const;
	FEMSInstanceSourceId BuildSourceId(
		const UInstancedStaticMeshComponent* Component) const;

	/** Registers every painted Static Mesh Foliage type the actor carries. */
	void CollectFoliageSources(AInstancedFoliageActor* FoliageActor);

	/** Recreates automatically generated replacement sources before discovery. */
	void EnsureSavedReplacementSources();

	/** Resolves a currently loaded source owner from its stable identity. */
	AActor* FindSourceOwner(const FEMSInstanceSourceId& SourceId) const;

	UInstancedStaticMeshComponent* FindTemplateComponent(
		const FEMSInstanceSourceId& SourceId) const;

	/** The class a generated target takes, which is never a Foliage module one. */
	static UClass* GetReplacementComponentClass(
		const UInstancedStaticMeshComponent* SourceComponent);

	UInstancedStaticMeshComponent* FindCompatibleReplacementSource(
		const FEMSInstanceSourceId& SourceId,
		const UInstancedStaticMeshComponent* SourceComponent,
		UStaticMesh* ReplacementMesh) const;

	UInstancedStaticMeshComponent* EnsureReplacementSource(
		const FEMSInstanceReplacementSource& Descriptor,
		UInstancedStaticMeshComponent* KnownTemplate = nullptr);

	UInstancedStaticMeshComponent* GetOrCreateReplacementSource(
		const FEMSInstanceSourceId& SourceId,
		UInstancedStaticMeshComponent* SourceComponent,
		UStaticMesh* ReplacementMesh);

	FName MakeReplacementSourceName(
		const FEMSInstanceSourceId& SourceId,
		const UStaticMesh* ReplacementMesh) const;

	static void ReadInstanceCustomData(
		const UInstancedStaticMeshComponent* Component,
		int32 InstanceIndex,
		TArray<float>& OutCustomData);
};
