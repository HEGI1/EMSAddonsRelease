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
};
