# Actor Spawner

Use **EMS Actor Spawner** for runtime actors that must belong to the exact streamed level or World Partition cell containing the spawner.

Default EMS remains the normal choice for runtime actors that can live in the persistent level.

## Basic use

1. Place an **EMS Actor Spawner** in the level or cell that should own the actors.
2. Call **Spawn Actor** on that spawner.
3. Use the returned actor and actor ID normally during gameplay.
4. Use **Destroy Spawned Actor** for permanent removal.
5. Save and load through EMS normally.

The spawner creates gameplay actors only when **Spawn Actor** is called or when EMS restores previously saved actors.

## Blueprint nodes

- **Spawn Actor** — spawns and registers a persistent runtime actor.
- **Destroy Spawned Actor** — permanently removes a spawned actor.
- **Get Spawned Actor** — finds the current actor from its saved actor ID.
- **Get All Spawned Actors** — returns currently spawned actors.
- **Get Spawned Actor ID** — gets the persistent ID for a spawned actor.
- **Was Spawned By This** — checks whether an actor belongs to this spawner.
- **Has Spawned Actor Record** — checks whether an ID still has a live or restorable record.
- **Has Finished Restoring** — reports whether the spawner restore pass has finished.
- **Get Last Restore Result** — returns the latest restore result.

## Important behavior

- Spawned actors use normal EMS actor binary data and do not need the EMS save interface.
- **Spawn Actor** returns no actor or ID while the spawner is capturing a save. Retry after saving completes; spawning from a child's save callback would otherwise create an incomplete saved record.
- A restored actor runs **Actor Pre Load** before its Construction Script and Begin Play, and **Actor Loaded** after its saved state is applied. If loading fails for a newly recreated actor, it is discarded and its record is retained for a later restore. An actor already live when restoration starts may remain live after a load failure; check **Get Last Restore Result** before relying on its restored state.
- An actor destroyed earlier in the session is restored by the next load that contains it, without waiting for garbage collection.
- Actors spawned through the spawner by restore, construction, or Begin Play callbacks remain live and are captured by a later save.
- The placed spawner defines level ownership. Under World Partition it is spatially loaded by default.
- Editor copy/paste, **Copy Actors to Level** and ordinary duplication assign the copied spawner a new identity. **Move to Level** and PIE preserve the authored identity so saved actors still match their spawner.
- **Max Spawned Actors** limits new records and records processed during restore.
- Attachment to the spawner is restored. Attachment between spawned actors should be rebuilt from **On Actor Restored**.
- **On Restore Started** and **On Restore Finished** expose the restore lifecycle. **On Actor Spawned** reports successful gameplay spawns, **On Actor Removed** reports gameplay removals, and **On Actor Restored** reports actors successfully restored.
- If a restored actor performs deferred addon restoration, use **Is EMS Addon Restore Complete** and **Get EMS Addon Restore Result** on that actor.
- Creation, permanent removal, capture, and restore are server-authoritative. Replication remains the project's responsibility.

Use [World Partition Runtime Actors](WORLD_PARTITION_RUNTIME_ACTORS.md) instead when actors should automatically follow World Partition streaming from their current location.
