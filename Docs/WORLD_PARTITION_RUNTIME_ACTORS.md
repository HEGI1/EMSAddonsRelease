# World Partition Runtime Actors

Use this addon for runtime actors that should automatically unload and return with World Partition based on their current location.

## Basic use

1. Implement the **EMS Actor Save Interface** on the actor class.
2. Spawn it with **Spawn World Partition Runtime Actor**.
3. Save and load through EMS normally.

Nothing needs to be placed. EMSAddons creates the persistent manager automatically.

The spawn location must be covered by a currently visible supported generated cell, or the spawn is rejected (see **Important behavior** below).

## Blueprint nodes

- **Spawn World Partition Runtime Actor** — spawns a managed runtime actor at the requested transform.
- **Get World Partition Runtime Actors** — the currently live managed actors. Actors whose location is unloaded are dormant and are not returned.
- **Is World Partition Runtime Actor Managed** — checks whether a live actor belongs to this system.
- **Is Managed Actor Removal In Progress** — true while EMSAddons is taking a managed actor out of the world. Read it from **End Play**.

No manual transfer or cell-management calls are required after spawning.

## Behavior

- The actor's current location determines its World Partition streaming lifetime.
- Managed actors travel freely between cells. They are not bound to the cell they were spawned in: a moving actor keeps being owned by wherever it currently is, so crossing cell boundaries needs no handover and no special handling.
- Several generated cells can cover one location. The actor normally stays live while any visible supported cell covers its location. Supporting-cell removal is an intentional exception: a character or simulating actor is captured before its supporting cell disappears even if another cell still covers the location.
- Characters and simulating physics actors are also captured before streamed geometry supporting them disappears, including level-owned BSP collision, and wait for that support cell before returning. If a later content update removes that cell from the generated grid, the actor returns as soon as its location is covered again instead of waiting forever.
- A dormant record that can never be restored, for example because its actor class was removed, logs one warning and stays in the save. It is not retried until the next load.
- When that location becomes available again, the actor is restored with its saved EMS state.
- Moving into an area that is not loaded is the one limit on travel. A low-frequency safety check captures such an actor, and it returns the same way.
- Normal EMS saves capture the actor at its current location.
- Gameplay destruction removes the persistent runtime record.

## Player-built structures

Stationary player-built structures can use the same system as traveling actors. Implement the **EMS Actor Save Interface** on the building class and create each building with **Spawn World Partition Runtime Actor**. No placed spawner is required.

The build location must be covered by a visible supported generated cell, and spawning must wait until EMS is not saving or loading. Handle a failed spawn in your building workflow.

A building does not need to move: its actor location determines when it becomes dormant and when it returns with its saved EMS state. It is not bound to its initial cell. If multiple supported cells cover that location, it normally stays live while any one is visible. Gameplay destruction removes its persistent record. A large building's footprint does not determine its streaming lifetime; the actor's location does.

For relationships between buildings, use soft references rather than hard actor references. A referenced building may be dormant, so handle an unresolved reference and resolve it again when the building is available. Stable actor names help with deterministic paths; hard references do not automatically rebind after restoration.

## NPCs

Managed Pawns work like other World Partition Runtime Actors. EMSAddons automatically cleans up an orphaned non-player Controller when a managed Pawn becomes dormant; restored Pawns use Unreal's normal **Auto Possess AI** behavior.

Keep persistent gameplay state on the Pawn and let the AI rebuild its runtime state from those loaded actor variables. For example, a StateTree can be started after **Actor Loaded** and simply derive its state from the restored variables instead of persisting the StateTree itself.

Set **Auto Possess AI** to **Spawned** or **Placed in World or Spawned** when the restored Pawn should automatically receive a Controller.

**Is Managed Actor Removal In Progress** is available when **End Play** needs to distinguish streaming removal from normal gameplay destruction.

## When to use it

Use **World Partition Runtime Actors** when a runtime actor should behave like spatial World Partition content without requiring a placed owner.

Use normal Easy Multi Save when the actor can remain in the persistent level.

Use [Actor Spawner](ACTOR_SPAWNER.md) when the actor must explicitly belong to a particular placed spawner, streamed level, or cell.

## Important behavior

- Spawn locations must be covered by a currently visible supported generated cell. This is checked once and not retried: spawning at BeginPlay before streaming has caught up (for example, at the position of an always-loaded actor whose cell has not streamed in yet) fails the spawn.
- If a managed actor temporarily cannot resolve a supported cell, it stays alive and is retried.
- When a generated cell begins hiding, that departing cell still counts as geometric coverage for the transition check but is excluded from the visible-cell result. This lets the last-covering-cell case become dormant immediately while an overlapping visible cell keeps the actor live. The one-second reconciliation remains a safety net for missed or out-of-order streaming notifications and for actors that move outside loaded coverage between events; it is no longer the normal fallback for the last-cell unload case.
- Spawning is rejected while a save or load is in progress, including the initial World Partition load. Gate spawns on **Is Saving Or Loading**, or wait for the EMS load to complete — an actor spawned into that window would be destroyed by the load that follows.
- The manager supports up to **4,096 runtime actor records**, including dormant actors. Additional spawns are rejected when this limit is reached.
- Loading replaces the managed runtime actors with the ones the save holds. Managed actors spawned since that save was written are destroyed and do not return. Actors destroyed since that save return as soon as their location is loaded, without waiting for garbage collection.
- The managed actor lives in the persistent level while active; EMSAddons supplies the World Partition streaming lifetime.
- A restored managed actor runs **Actor Pre Load** before its Construction Script and Begin Play, and **Actor Loaded** after its saved state is applied.
- Stable names help deterministic paths and soft references. Hard references do not automatically rebind after restoration.
- HLOD and Data-Layer-specific ownership are outside the supported scope.
- The system is server-authoritative. Replication remains the project's responsibility.
