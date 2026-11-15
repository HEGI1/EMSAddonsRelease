//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSCustomSaveGame.h"
#include "EMSAutosaveTypes.h"
#include "EMSSaveSlotMetadata.generated.h"

/** Internal slot-bound menu metadata. Never carried from another save's progress. */
UCLASS()
class EMSADDONSAUTOSAVE_API UEMSSaveSlotMetadata : public UEMSCustomSaveGame
{
	GENERATED_BODY()

public:
	UEMSSaveSlotMetadata();

	UPROPERTY(SaveGame)
	FText DisplayName;

	UPROPERTY(SaveGame)
	EEMSSaveSlotType SaveType = EEMSSaveSlotType::Autosave;

	/**
	 * Id of the last addon save that finished writing this slot.
	 *
	 * The metadata is the final write of a save. A recovery journal naming the same
	 * id therefore describes a save that completed, whatever the journal itself says.
	 */
	UPROPERTY(SaveGame)
	FString CompletedSaveId;
};
