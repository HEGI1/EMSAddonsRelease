//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"

struct FWorldContext;

namespace EMSAutosaveMapTravel
{
	/** Handles the engine's pre-load-map boundary while the outgoing world is still valid. */
	void HandlePreLoadMap(const FWorldContext& WorldContext, const FString& MapName);
}
