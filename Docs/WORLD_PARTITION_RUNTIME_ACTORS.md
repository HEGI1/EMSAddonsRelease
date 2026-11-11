# World Partition Runtime Actors

Use **World Partition Runtime Actors** for runtime-spawned actors that should automatically unload and return according to their *current* World Partition location. This works for player-built structures and moving NPCs.

## Basic use

1. Implement the **EMS Actor Save Interface** on the actor class.
2. Call **Spawn World Partition Runtime Actor** at a location in a **currently loaded World Partition cell**.
3. Save and load normally through EMS.

No placed spawner is needed. The spawn node fails if the location is not yet covered by a loaded cell or EMS is busy; handle failure and retry later.

## Blueprint nodes

- **Spawn World Partition Runtime Actor** — creates a managed actor.
- **Get World Partition Runtime Actors** — returns currently active managed actors.
- **Is World Partition Runtime Actor Managed** — checks whether an actor is managed.
- **Is Managed Actor Removal In Progress** — use in **End Play** to distinguish streaming removal from gameplay destruction. Also supports placed actors.
- **Get Managed Runtime Actor Count** / **Get Dormant Runtime Actor Count** — diagnostic counts of active and unloaded records.

## Important behavior

- Actors move freely between cells. Their current location determines when they unload and return.
- A character or simulating actor can unload when the streamed floor supporting it disappears, even if another cell covers its position.
- When the required area or supporting geometry returns, an actor is recreated with its saved state.
- Gameplay destruction is persistent. Loading an earlier save can restore an actor that was alive in that save.
- For player-built structures, spawn the finished building with the runtime-actor node instead of a normal actor spawn.
- For NPCs, set **Auto Possess AI** to **Spawned** or **Placed in World or Spawned** if restored Pawns need an AI Controller. Rebuild transient AI behavior after **Actor Loaded**.
- Use soft references or persistent IDs for relationships; hard references are not automatically rebound after recreation.
- A class carrying **EMS World Partition Placed Component** cannot be spawned by this node. Use the placed-actor workflow instead.
- Up to **4,096** runtime actor records are supported.
- HLOD and runtime Data-Layer ownership are outside the supported scope. Restoration is server-authoritative; replication is the project's responsibility.

## Alternatives

Use [Actor Spawner](ACTOR_SPAWNER.md) if a runtime actor must belong to a specific streamed level or cell. Use normal Easy Multi Save if it can stay persistent without location-based World Partition streaming.

## Settings

Runtime actors have no addon settings.
