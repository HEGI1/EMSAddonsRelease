//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAddonsTypes.h"
#include "UObject/Interface.h"
#include "EMSAddonRestoreParticipant.generated.h"

UINTERFACE(BlueprintType, meta = (DisplayName = "EMS Addon Restore Participant"))
class EMSADDONSCORE_API UEMSAddonRestoreParticipant : public UInterface
{
	GENERATED_BODY()
};

/**
 * Reports the progress of a restore the implementer starts on its own.
 *
 * Query only. An addon type that defers restoration past its EMS Actor Loaded
 * event drives that itself, so there is no entry point to start or restart one:
 * doing so from outside would race the load pass the state comes from.
 *
 * Deliberately plain C++ rather than Blueprint Native Events. A Blueprint Native
 * Event declared on an interface dispatches through the interface's own UFunction,
 * whose generated thunk casts the object pointer without applying the interface
 * offset, so Execute_ returned an uninitialized value for every Actor that
 * implements this. Blueprint reaches these through the nodes in
 * EMSAddonRestoreLibrary, which resolve the implementer with an ordinary Cast.
 */
class EMSADDONSCORE_API IEMSAddonRestoreParticipant
{
	GENERATED_BODY()

public:
	virtual bool IsEMSAddonRestoreComplete() const
	{
		return true;
	}

	virtual FEMSAddonResult GetEMSAddonRestoreResult() const
	{
		return FEMSAddonResult::Success();
	}
};
