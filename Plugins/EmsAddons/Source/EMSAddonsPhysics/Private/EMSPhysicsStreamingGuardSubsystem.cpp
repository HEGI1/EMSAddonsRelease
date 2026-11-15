//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSPhysicsStreamingGuardSubsystem.h"

#include "Components/PrimitiveComponent.h"
#include "EMSAddonsPhysics.h"
#include "EMSPhysicsStreamingGuardComponent.h"
#include "EMSPhysicsStreamingGuardSettings.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"

namespace
{
	bool HasSimulatingBody(const AActor* Actor)
	{
		for (const UActorComponent* Component : Actor->GetComponents())
		{
			const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
			if (Primitive && Primitive->BodyInstance.bSimulatePhysics)
			{
				return true;
			}
		}
		return false;
	}
}

bool UEMSPhysicsStreamingGuardSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UEMSPhysicsStreamingGuardSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// A client never adds guards: the movement it could hold is the server's.
	if (InWorld.GetNetMode() == NM_Client)
	{
		return;
	}

	if (UGameInstance* GameInstance = InWorld.GetGameInstance())
	{
		GameInstance->GetOnPawnControllerChanged().AddUniqueDynamic(
			this, &UEMSPhysicsStreamingGuardSubsystem::HandlePawnControllerChanged);
		BoundGameInstance = GameInstance;
	}

	// Pawns placed with Auto Possess, or possessed while the world was starting,
	// changed controller before this subsystem was listening.
	for (TActorIterator<APawn> It(&InWorld); It; ++It)
	{
		GuardPawnIfWanted(*It);
	}
}

void UEMSPhysicsStreamingGuardSubsystem::Deinitialize()
{
	if (UGameInstance* GameInstance = BoundGameInstance.Get())
	{
		GameInstance->GetOnPawnControllerChanged().RemoveDynamic(
			this, &UEMSPhysicsStreamingGuardSubsystem::HandlePawnControllerChanged);
	}
	BoundGameInstance.Reset();
	Super::Deinitialize();
}

void UEMSPhysicsStreamingGuardSubsystem::HandlePawnControllerChanged(APawn* Pawn, AController* Controller)
{
	// The game instance outlives this world and reports pawns of every world it hosts.
	if (Pawn && Pawn->GetWorld() == GetWorld())
	{
		GuardPawnIfWanted(Pawn);
	}
}

UEMSPhysicsStreamingGuardComponent* UEMSPhysicsStreamingGuardSubsystem::GuardPawnIfWanted(APawn* Pawn)
{
	if (!IsValid(Pawn) || Pawn->IsActorBeingDestroyed() || Pawn->GetWorld() != GetWorld() || !Pawn->HasAuthority())
	{
		return nullptr;
	}
	if (UEMSPhysicsStreamingGuardComponent* Existing = Pawn->FindComponentByClass<UEMSPhysicsStreamingGuardComponent>())
	{
		return Existing;
	}

	const AController* Controller = Pawn->GetController();
	if (!Controller)
	{
		return nullptr;
	}

	const UEMSPhysicsStreamingGuardSettings* Settings = GetDefault<UEMSPhysicsStreamingGuardSettings>();
	const bool bHasCharacterMovement = Pawn->FindComponentByClass<UCharacterMovementComponent>() != nullptr;
	if (Controller->IsPlayerController())
	{
		// A spectator or other free-flying pawn has nothing gravity pulls down.
		if (!Settings->bGuardPlayerPawns || (!bHasCharacterMovement && !HasSimulatingBody(Pawn)))
		{
			return nullptr;
		}
	}
	else if (!Settings->bGuardAICharacters || !bHasCharacterMovement)
	{
		return nullptr;
	}

	// Transient: the guard is a runtime safeguard and nothing about it is saved.
	// Registering on a pawn that has begun play also begins play on the guard,
	// which runs the startup check for world that has not streamed in yet.
	UEMSPhysicsStreamingGuardComponent* Guard = NewObject<UEMSPhysicsStreamingGuardComponent>(
		Pawn,
		MakeUniqueObjectName(Pawn, UEMSPhysicsStreamingGuardComponent::StaticClass(), TEXT("EMSAutoPhysicsStreamingGuard")),
		RF_Transient);
	Guard->RegisterComponent();
	UE_LOG(
		LogEMSAddonsPhysics,
		Verbose,
		TEXT("EMS physics streaming guard added automatically. Pawn=%s"),
		*Pawn->GetPathName());
	return Guard;
}
