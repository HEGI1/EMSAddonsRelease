//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAutosaveTypes.h"

class UEMSCheckpointSaveGame;
class UWorld;

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

	/**
	 * Reads the checkpoint a checkpoint custom save names, as a current (version 2) record.
	 *
	 * A 1.1 record names no slot: its checkpoint is HolderSlot itself, and only while its
	 * two generations match (otherwise a later 1.1 save had superseded it). It is returned
	 * with Save Slot set to HolderSlot. Returns false when there is no valid checkpoint.
	 */
	EMSADDONSAUTOSAVE_API bool ResolveRecord(
		const UEMSCheckpointSaveGame* Save,
		const FString& HolderSlot,
		FEMSCheckpointRecord& OutRecord);

	/** True when the save holds a 1.1 record that ResolveRecord accepts. */
	EMSADDONSAUTOSAVE_API bool IsLoadableLegacyRecord(
		const UEMSCheckpointSaveGame* Save);
}
