//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "EMSPhysicsStreamingGuardSubsystem.generated.h"

class AController;
class APawn;
class UGameInstance;
class UEMSPhysicsStreamingGuardComponent;

/**
 * Adds an EMS Physics Streaming Guard to player pawns and AI characters.
 *
 * Possession is the moment a pawn becomes one or the other, so the subsystem
 * listens for controller changes and also sweeps the pawns that were possessed
 * before the world began play. Which pawns qualify is set in the Physics
 * Streaming Guard project settings.
 */
UCLASS()
class EMSADDONSPHYSICS_API UEMSPhysicsStreamingGuardSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	/**
	 * Adds a guard to Pawn when the project settings ask for one.
	 * Returns the guard the pawn ends up with, its own or an added one, or null.
	 */
	UEMSPhysicsStreamingGuardComponent* GuardPawnIfWanted(APawn* Pawn);

private:
	TWeakObjectPtr<UGameInstance> BoundGameInstance;

	UFUNCTION()
	void HandlePawnControllerChanged(APawn* Pawn, AController* Controller);
};
