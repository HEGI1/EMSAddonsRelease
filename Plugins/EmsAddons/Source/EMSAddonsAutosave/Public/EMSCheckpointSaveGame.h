//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSCustomSaveGame.h"
#include "EMSAutosaveTypes.h"
#include "EMSCheckpointSaveGame.generated.h"

/** Internal EMS custom-save object that stores checkpoint metadata per EMS save slot. */
UCLASS()
class EMSADDONSAUTOSAVE_API UEMSCheckpointSaveGame : public UEMSCustomSaveGame
{
	GENERATED_BODY()

public:
	UEMSCheckpointSaveGame();

	/** True when this slot contains a committed checkpoint record. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Metadata")
	bool bHasCheckpoint = false;

	/** Latest committed checkpoint for this EMS save slot. */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Metadata")
	FEMSCheckpointRecord LatestCheckpoint;

	/**
	 * Generation of the newest EMS save started by the checkpoint system for this slot.
	 * A generation is reserved before the EMS save begins so an interrupted two-file
	 * checkpoint commit can be detected instead of pairing stale metadata with newer state.
	 */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Metadata")
	FGuid SaveGeneration;

	/**
	 * Generation for which LatestCheckpoint was committed.
	 * It must match SaveGeneration before the checkpoint record is considered loadable.
	 */
	UPROPERTY(SaveGame, VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons|Autosave and Checkpoints|Metadata")
	FGuid CheckpointGeneration;
};
