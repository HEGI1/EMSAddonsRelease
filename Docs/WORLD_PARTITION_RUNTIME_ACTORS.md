# World Partition Runtime Actors

Use **Spawn World Partition Runtime Actor** for spawned actors that move around a World Partition map, such as player-built structures or wandering NPCs. They unload when their current area streams out and return, with their saved state, when it streams back in.

## Quick start

1. Add the **EMS Actor Save Interface** to the actor class.
2. Call **Spawn World Partition Runtime Actor** at a location inside a loaded cell.
3. Save and load normally through EMS.

Nothing needs to be placed. The spawn fails if the location isn't loaded yet or EMS is saving or loading, so handle failure and retry later.

## Good to know

- Actors move freely between cells. Their current position decides when they unload.
- A character or physics actor also unloads when the floor under it streams out.
- Destroying an actor during gameplay is permanent. Loading an earlier save brings it back if it was alive then.
- Link actors with **soft references** or saved IDs. Hard references aren't reconnected after an actor is recreated.
- **NPCs:** set **Auto Possess AI** to **Spawned** or **Placed in World or Spawned** so restored Pawns get an AI Controller. Restart AI behavior in **Actor Loaded**.
- In **End Play**, use **Is Managed Actor Removal In Progress** to tell streaming removal apart from gameplay destruction.
- Up to 4,096 actors per map. HLOD and Runtime Data Layers aren't supported.
- Saving and restoration run on the server or in standalone. Replication is your project's responsibility.

## Multiple maps and older saves

Each map keeps its own actors. To keep several World Partition maps in one save slot, set EMS **Multi-Level Saving** to **Basic** or **Full**. With **Disabled** or **Streaming**, EMS keeps only the current map's data.

**Saves from before 1.3.0** are not restored for these actors, and a warning is logged. Save again to write data in the new format.

## Which addon?

- Actor should stream with its **current position** → this addon.
- Actor should belong to **one specific level or cell** → [Actor Spawner](ACTOR_SPAWNER.md).
- Actor is **placed in the editor** → [World Partition Placed Actors](WORLD_PARTITION_PLACED_ACTORS.md).

## Blueprint nodes

These are library nodes; no subsystem reference is needed.

| Task | Nodes |
| --- | --- |
| Spawn an actor | **Spawn World Partition Runtime Actor** |
| List loaded actors | **Get World Partition Runtime Actors** |
| Check an actor | **Is World Partition Runtime Actor Managed**, **Is Managed Actor Removal In Progress** |
| Count loaded and unloaded actors | **Get Managed Runtime Actor Count**, **Get Dormant Runtime Actor Count** |

Classes with an **EMS World Partition Placed Component** can't be spawned this way.

This addon has no settings.
