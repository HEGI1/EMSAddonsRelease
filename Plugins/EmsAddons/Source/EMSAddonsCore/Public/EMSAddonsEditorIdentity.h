//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace EMSAddons
{

/**
 * Keep a moved actor's saved identity, but give an imported copy its own.
 * Use in both PostEditImport and PostDuplicate: editor paste calls both
 * before clearing CopyPasteId, including when moving actors between levels.
 */
template <typename ActorType>
bool ShouldRegenerateImportedIdentity(const ActorType* Imported, const FGuid ActorType::* IdentityMember)
{
	if (Imported->HasAnyFlags(RF_ClassDefaultObject))
	{
		return false;
	}
	if (Imported->CopyPasteId == static_cast<uint32>(INDEX_NONE))
	{
		return true;
	}

	// Copy Actors to Level and Move Actors to Level both set CopyPasteId.
	// A move deletes its source before import; a copy leaves a live source.
	// Include hidden loaded levels as well as the active destination level.
	if (const UWorld* World = Imported->GetWorld())
	{
		for (TActorIterator<ActorType> It(World, ActorType::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It)
		{
			const ActorType* Other = *It;
			if (Other != Imported && !Other->IsActorBeingDestroyed()
				&& Other->*IdentityMember == Imported->*IdentityMember)
			{
				return true;
			}
		}
	}
	return false;
}

}
#endif
