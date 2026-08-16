//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAddonsTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "EMSAddonRestoreLibrary.generated.h"

/**
 * The Blueprint side of EMS Addon Restore Participant.
 *
 * One node pair for every addon that restores asynchronously. A target that does
 * not implement the interface reports a finished, successful restore, so a
 * Blueprint can ask any actor without branching on its type first.
 */
UCLASS()
class EMSADDONSCORE_API UEMSAddonRestoreLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(
		BlueprintPure,
		Category = "EMS Addons|Restore",
		meta = (DisplayName = "Is EMS Addon Restore Complete", DefaultToSelf = "Target"))
	static bool IsEMSAddonRestoreComplete(const UObject* Target);

	UFUNCTION(
		BlueprintPure,
		Category = "EMS Addons|Restore",
		meta = (DisplayName = "Get EMS Addon Restore Result", DefaultToSelf = "Target"))
	static FEMSAddonResult GetEMSAddonRestoreResult(const UObject* Target);
};
