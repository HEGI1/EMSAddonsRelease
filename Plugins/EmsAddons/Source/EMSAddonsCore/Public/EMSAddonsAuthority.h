//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"

namespace EMSAddons
{
	/** Shared net-mode rule for addon persistence. */
	inline bool HasPersistenceAuthority(const ENetMode NetMode)
	{
		return NetMode != NM_Client;
	}

	/**
	 * Whether this actor may capture or restore persistent addon state.
	 *
	 * The net mode is what decides client rejection, not HasAuthority alone. None
	 * of the addon workflows replicate their saved state, and a non-replicated
	 * actor reports ROLE_Authority on a client too, so HasAuthority by itself would
	 * let a client mutate persistent state locally and diverge from the save the
	 * server keeps.
	 *
	 * The explicit net mode exists so workflows that isolate it behind a virtual
	 * accessor, which is how automation exercises every net mode from one editor
	 * world, still share this one rule.
	 */
	inline bool HasPersistenceAuthority(const AActor* Actor, const ENetMode NetMode)
	{
		return Actor
			&& HasPersistenceAuthority(NetMode)
			&& Actor->HasAuthority();
	}

	inline bool HasPersistenceAuthority(const AActor* Actor)
	{
		return Actor && HasPersistenceAuthority(Actor, Actor->GetNetMode());
	}
}
