//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"

struct FGameObjectSaveData;

namespace EMSAddons
{
	/** Packs EMS actor data using the format historically owned by Actor Spawner. */
	EMSADDONSCORE_API bool PackActorSaveData(const FGameObjectSaveData& SaveData, TArray<uint8>& OutBinary);

	/** Validates and unpacks an EMSAddons actor-data envelope. */
	EMSADDONSCORE_API bool UnpackActorSaveData(const TArray<uint8>& Binary, FGameObjectSaveData& OutSaveData);
}
