//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAutosaveTypes.h"

class UEMSCheckpointSaveGame;
class UWorld;

/**
 * Checkpoint activation writes two files that cannot be written at once: the
 * EMS player/level save, then the checkpoint metadata. If the second write
 * never lands, the slot holds new world state described by an older checkpoint.
 *
 * A generation ID pairs the two. It is reserved in metadata before the checkpoint
 * EMS save runs and confirmed after that save succeeds. Later normal EMS actor
 * saves clear the checkpoint metadata, so a checkpoint is never presented as an
 * independent snapshot of newer slot data.
 */
namespace EMSCheckpoint
{
	/**
	 * Stable asset path of a world, with any PIE prefix removed.
	 * Checkpoint records store this path, so recording and matching must derive it identically.
	 */
	EMSADDONSAUTOSAVE_API FSoftObjectPath GetWorldAssetPath(
		const UWorld* World);

	/** True when the supplied world is the world a checkpoint record refers to. */
	EMSADDONSAUTOSAVE_API bool IsSameWorld(
		const UWorld* World,
		const FSoftObjectPath& WorldPath);

	/** True while the stored checkpoint record still describes the EMS actor save paired with it. */
	EMSADDONSAUTOSAVE_API bool IsCurrentGeneration(
		const UEMSCheckpointSaveGame* Save);

	/**
	 * Reserves the generation of the EMS save that is about to run.
	 * A checkpoint record already known to be stale is discarded by a successful reservation
	 * so a later failed save cannot revive it. A failed reservation write restores the exact prior metadata.
	 */
	EMSADDONSAUTOSAVE_API bool BeginGeneration(
		UEMSCheckpointSaveGame* Save,
		const FGuid& Generation,
		TFunctionRef<bool()> WriteToDisk);

	/** Stages a checkpoint record and rolls the object back if the supplied metadata write fails. */
	EMSADDONSAUTOSAVE_API bool CommitRecord(
		UEMSCheckpointSaveGame* Save,
		const FEMSCheckpointRecord& Record,
		const FGuid& Generation,
		TFunctionRef<bool()> WriteToDisk);

	/**
	 * Rolls back a reserved generation only when the associated EMS save task never
	 * started. Once the EMS task has started, the reservation must remain unresolved
	 * on failure because one of EMS's separate core files may already have changed.
	 */
	EMSADDONSAUTOSAVE_API bool AbandonGeneration(
		UEMSCheckpointSaveGame* Save,
		TFunctionRef<bool()> WriteToDisk);

	/**
	 * Clears a currently committed checkpoint after a later normal EMS actor save
	 * superseded its state. Rolls the metadata object back if its disk write fails.
	 */
	EMSADDONSAUTOSAVE_API bool ClearCurrentCheckpoint(
		UEMSCheckpointSaveGame* Save,
		TFunctionRef<bool()> WriteToDisk);
}
