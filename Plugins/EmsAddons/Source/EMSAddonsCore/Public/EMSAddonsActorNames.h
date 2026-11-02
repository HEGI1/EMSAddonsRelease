//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"

class ULevel;

namespace EMSAddons
{
	/**
	 * Frees a stable actor name that a destroyed actor still holds in Level.
	 *
	 * A destroyed actor keeps its name until garbage collection, and SpawnActor
	 * treats that name as taken. A load that restores the same record before the
	 * next collection would otherwise fail to respawn it. Returns false only when
	 * a live object holds the name, or when the destroyed one cannot be renamed.
	 */
	EMSADDONSCORE_API bool ReleaseDestroyedActorName(ULevel* Level, FName Name);
}
