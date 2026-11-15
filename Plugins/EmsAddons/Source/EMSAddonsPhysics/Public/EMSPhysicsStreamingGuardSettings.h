//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "EMSPhysicsStreamingGuardSettings.generated.h"

/**
 * Which actors receive an EMS Physics Streaming Guard without one being added by hand.
 *
 * A guard is only ever added on the server or in standalone, to a pawn that has
 * none yet. Actors that already carry a guard keep their own.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Physics Streaming Guard"))
class EMSADDONSPHYSICS_API UEMSPhysicsStreamingGuardSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UEMSPhysicsStreamingGuardSettings();

	/**
	 * Adds a guard to every pawn a player possesses.
	 * Keeps the player from falling through World Partition cells or sublevels that have not
	 * streamed in yet, at startup and after a teleport, EMS load or checkpoint restore.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Automatic Guards")
	bool bGuardPlayerPawns = true;

	/**
	 * Adds a guard to every Character possessed by an AI controller.
	 * Holds the character in place while the ground under it streams out or has not streamed in yet.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Automatic Guards")
	bool bGuardAICharacters = true;
};
