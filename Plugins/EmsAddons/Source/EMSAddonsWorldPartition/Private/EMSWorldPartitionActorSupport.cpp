//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSWorldPartitionActorSupport.h"
#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "EMSWorldPartitionCellResolver.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Pawn.h"
#include "WorldPartition/WorldPartitionLevelStreamingDynamic.h"
#include "WorldPartition/WorldPartitionRuntimeLevelStreamingCell.h"

namespace
{
	/** Levels that can support the actor: a pawn's movement base and a simulating body's resting contact. */
	void GetSupportLevels(UWorld* World, AActor* Actor, const ULevel*& OutBaseLevel, const ULevel*& OutContactLevel)
	{
		OutBaseLevel = nullptr;
		OutContactLevel = nullptr;

		//A Pawn riding a moving base (lift, vehicle) is supported by that base's Level.
		if (const APawn* Pawn = Cast<APawn>(Actor))
		{
			if (const AActor* BaseActor = APawn::GetMovementBaseActor(Pawn))
			{
				OutBaseLevel = BaseActor->GetLevel();
			}
			else if (const UPrimitiveComponent* BaseComponent =
				Cast<UPrimitiveComponent>(Pawn->GetMovementBaseObject()))
			{
				// Level-owned collision has no base actor (for example a model
				// component). Its level still supplies the character's support.
				OutBaseLevel = BaseComponent->GetComponentLevel();
			}
		}

		const UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
		if (!RootPrimitive || !RootPrimitive->IsSimulatingPhysics())
		{
			return;
		}

		//Otherwise, trace a short distance below the bounds for resting contact: 2 units of
		//overlap tolerance plus 20 units of reach, enough to catch normal physics settling
		//jitter without reaching far enough to hit unrelated geometry below a gap.
		FVector BoundsOrigin;
		FVector BoundsExtent;
		Actor->GetActorBounds(true, BoundsOrigin, BoundsExtent, false);
		const FVector Start(BoundsOrigin.X, BoundsOrigin.Y, BoundsOrigin.Z - BoundsExtent.Z + 2.0);
		const FVector End = Start - FVector(0.0, 0.0, 22.0);
		FHitResult Hit;
		const FCollisionQueryParams QueryParams(NAME_None, false, Actor);
		const FCollisionObjectQueryParams ObjectParams(
			ECC_TO_BITFIELD(ECC_WorldStatic)
			| ECC_TO_BITFIELD(ECC_WorldDynamic)
			| ECC_TO_BITFIELD(ECC_PhysicsBody));
		if (World->LineTraceSingleByObjectType(Hit, Start, End, ObjectParams, QueryParams))
		{
			if (const UPrimitiveComponent* HitComponent = Hit.GetComponent())
			{
				OutContactLevel = HitComponent->GetComponentLevel();
			}
		}
	}
}

FGuid EMSAddonsWorldPartition::GetCurrentSupportCell(UWorld* World, AActor* Actor)
{
	if (!IsValid(World) || !IsValid(Actor)) return FGuid();
	const UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
	if (!Actor->IsA<APawn>() && (!Root || !Root->IsSimulatingPhysics())) return FGuid();
	// Resolve support once, then match it to a cell. Runtime capture runs for every
	// managed actor on each save, so a trace per visible cell would scale poorly.
	const ULevel* BaseLevel;
	const ULevel* ContactLevel;
	GetSupportLevels(World, Actor, BaseLevel, ContactLevel);
	if (!BaseLevel && !ContactLevel) return FGuid();
	for (const ULevelStreaming* Streaming : World->GetStreamingLevels())
	{
		const auto* WPStreaming = Cast<UWorldPartitionLevelStreamingDynamic>(Streaming);
		const auto* Cell = WPStreaming
			? Cast<UWorldPartitionRuntimeLevelStreamingCell>(WPStreaming->GetWorldPartitionRuntimeCell()) : nullptr;
		const ULevel* Level = WPStreaming ? WPStreaming->GetLoadedLevel() : nullptr;
		if (IsSupportedCell(Cell) && Cell->IsVisible() && IsValid(Level)
			&& (Level == BaseLevel || Level == ContactLevel)) return Cell->GetGuid();
	}
	return FGuid();
}

bool EMSAddonsWorldPartition::IsActorSupportedByLevel(UWorld* World, AActor* Actor, const ULevel* Level)
{
	if (!IsValid(World) || !IsValid(Actor) || !IsValid(Level))
	{
		return false;
	}
	const ULevel* BaseLevel;
	const ULevel* ContactLevel;
	GetSupportLevels(World, Actor, BaseLevel, ContactLevel);
	return Level == BaseLevel || Level == ContactLevel;
}
