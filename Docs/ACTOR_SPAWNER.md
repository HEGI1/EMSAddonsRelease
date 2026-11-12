# Actor Spawner

Use **EMS Actor Spawner** for runtime actors that belong to one streamed level or World Partition cell. The actors stream out and back in with the spawner. For actors that should follow their *current* location instead, use [World Partition Runtime Actors](WORLD_PARTITION_RUNTIME_ACTORS.md).

## Quick start

1. Place an **EMS Actor Spawner** in the level or cell that should own the actors.
2. Call **Spawn Actor** on it. Keep the returned ID if you need to find the actor later.
3. Call **Destroy Spawned Actor** to remove one permanently.
4. Save and load normally through EMS.

Spawned actors don't need the EMS Actor Save Interface; the spawner saves and restores them.

## Good to know

- **Spawn Actor** fails while EMS is saving or loading. Spawning from the spawner's own restore events is fine.
- Restored actors get the normal EMS load callbacks.
- Attachment to the spawner is restored. Re-attach spawned actors to each other in **On Actor Restored**.
- Duplicating a spawner in the editor gives the copy a new identity. Moving it to another level keeps its identity.
- Spawning, removal, and restoration run on the server or in standalone. Replication is your project's responsibility.

## Blueprint nodes

Call these on **EMS Actor Spawner**.

| Task | Nodes |
| --- | --- |
| Spawn or remove | **Spawn Actor**, **Destroy Spawned Actor** |
| Find actors | **Get Spawned Actor**, **Get All Spawned Actors**, **Get Spawned Actor Id** |
| Check ownership | **Was Spawned By This**, **Has Spawned Actor Record** |
| Check restoration | **Has Finished Restoring**, **Get Last Restore Result** |
| Read the spawner's ID | **Get Spawner Identity** |

## Events

| Event | Meaning |
| --- | --- |
| **On Actor Spawned** | **Spawn Actor** created an actor. |
| **On Actor Removed** | A spawned actor was destroyed during gameplay. |
| **On Restore Started** | The spawner began restoring its saved actors. |
| **On Actor Restored** | One actor was restored. Rebuild links between actors here. |
| **On Restore Finished** | Restoration ended. Check the result for failed or skipped actors. |

Some actors finish restoring later, such as an **EMS Level Sequence Actor** or **EMS Geometry Collection Actor**. If that matters, check them with **Is EMS Addon Restore Complete**.

## Settings

| Setting | Default | Purpose |
| --- | --- | --- |
| **Max Spawned Actors** | 1,024 | How many actors this spawner manages. |
