//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once
#include "CoreMinimal.h"
class AActor;
class UWorld;
class ULevel;
namespace EMSAddonsWorldPartition
{
    bool IsActorSupportedByLevel(UWorld* World, AActor* Actor, const ULevel* Level);
    /** Visible generated cell currently supporting a pawn or simulating body. */
    FGuid GetCurrentSupportCell(UWorld* World, AActor* Actor);
}
