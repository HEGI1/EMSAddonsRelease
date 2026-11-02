//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAddonsActorNames.h"

#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"

bool EMSAddons::ReleaseDestroyedActorName(ULevel* Level, const FName Name)
{
	if (!Level || Name.IsNone())
	{
		return false;
	}

	// The same lookup SpawnActor uses, which also finds objects that are only
	// waiting for garbage collection.
	UObject* ExistingObject = StaticFindObjectFast(nullptr, Level, Name);
	if (!ExistingObject)
	{
		return true;
	}
	AActor* DestroyedActor = Cast<AActor>(ExistingObject);
	if (!DestroyedActor || IsValid(DestroyedActor))
	{
		return false;
	}

	const FString RetiredName = FString::Printf(
		TEXT("EMS_ReleasedActor_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	return DestroyedActor->Rename(
		*RetiredName,
		nullptr,
		REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
}
