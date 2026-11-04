# Actor Spawner

Use **EMS Actor Spawner** for runtime actors that belong to a specific streamed level or World Partition cell. For actors that follow their *current* World Partition location, use [World Partition Runtime Actors](WORLD_PARTITION_RUNTIME_ACTORS.md).

## Basic use

1. Place an **EMS Actor Spawner** in the level or cell that should own the actors.
2. Call **Spawn Actor** on that spawner.
3. Keep the returned actor ID if you need to find or remove the actor later.
4. Call **Destroy Spawned Actor** for permanent removal.
5. Save and load normally through EMS.

Spawned actors do **not** need the EMS Actor Save Interface. The spawner saves their state and restores them when its level or cell returns.

## Blueprint nodes

- **Spawn Actor** — creates and registers an actor.
- **Destroy Spawned Actor** — permanently removes a registered actor.
- **Get Spawned Actor** — finds a currently spawned actor by its saved ID.
- **Get All Spawned Actors** — returns currently spawned actors.
- **Get Spawned Actor ID** — gets the ID of a spawned actor.
- **Was Spawned By This** — checks spawner ownership.
- **Has Spawned Actor Record** — checks whether a saved ID is still registered.
- **Has Finished Restoring** / **Get Last Restore Result** — check restore status.

## Events

- **On Actor Spawned** / **On Actor Removed**
- **On Restore Started** / **On Actor Restored** / **On Restore Finished**

Use **On Actor Restored** to rebuild relationships between spawned actors when necessary.

## Important behavior

- The spawner controls where its actors stream in and out. It is spatially loaded by default in World Partition.
- **Spawn Actor** can fail during an EMS save or load. Avoid ordinary gameplay spawns while EMS is busy; the spawner's own restore callbacks can perform reentrant work during restoration.
- **Max Spawned Actors** limits how many records the spawner can manage.
- Duplicating a spawner in the editor gives the copy a new identity. Moving it to another level preserves its identity.
- Attachment to the spawner is restored. Attachments between spawned actors should be re-established after restoration.
- Restored actors receive normal EMS load callbacks. Check **Get Last Restore Result** before depending on data from a failed restore.
- Spawning, removal, and restoration are server-authoritative; replication is the project's responsibility.
