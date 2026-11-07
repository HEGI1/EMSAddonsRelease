//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSCustomSaveGame.h"
#include "EMSAutosaveTypes.h"
#include "EMSCheckpointSaveGame.generated.h"

/**
 * Internal slot-bound EMS custom save naming the checkpoint that belongs to a slot's progress.
 *
 * A checkpoint save writes it into its own checkpoint slot. Every later addon save carries it
 * into the slot it writes, so any save knows which checkpoint Load Last Checkpoint returns to.
 */
UCLASS()
class EMSADDONSAUTOSAVE_API UEMSCheckpointSaveGame : public UEMSCustomSaveGame
{
	GENERATED_BODY()

public:
	UEMSCheckpointSaveGame();

	/** True when this slot's progress has a checkpoint. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Metadata")
	bool bHasCheckpoint = false;

	/** The checkpoint of this slot's progress. Its Save Slot names the slot holding the checkpoint save. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Metadata")
	FEMSCheckpointRecord LatestCheckpoint;

	/**
	 * Read from 1.1 saves only. A 1.1 checkpoint lives in the player's own slot and is still
	 * loadable while these two match. Version 2 records clear them.
	 */
	UPROPERTY(SaveGame)
	FGuid SaveGeneration;

	/** Read from 1.1 saves only. See Save Generation. */
	UPROPERTY(SaveGame)
	FGuid CheckpointGeneration;
};
