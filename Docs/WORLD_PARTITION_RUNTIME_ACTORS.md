# World Partition Runtime Actors

Use this addon for runtime actors that should automatically unload and return with World Partition based on their current location.

## Basic use

1. Implement the **EMS Actor Save Interface** on the actor class.
2. Spawn it with **Spawn World Partition Runtime Actor**.
3. Save and load through EMS normally.

Nothing needs to be placed. EMSAddons creates the persistent manager automatically.

## Blueprint nodes

- **Spawn World Partition Runtime Actor** — spawns a managed runtime actor at the requested transform.
- **Get World Partition Runtime Actors** — the currently live managed actors. Actors whose location is unloaded are dormant and are not returned.
- **Is Managed Actor Removal In Progress** — true while EMSAddons is taking a managed actor out of the world. Read it from **End Play**.

No manual transfer or cell-management calls are required after spawning.

## Behavior

- The actor's current location determines its World Partition streaming lifetime.
- Managed actors travel freely between cells. They are not bound to the cell they were spawned in: a moving actor keeps being owned by wherever it currently is, so crossing cell boundaries needs no handover and no special handling.
- Several generated cells can cover one location. The actor stays live for as long as any visible supported cell covers its location, and is captured only when the last one unloads.
- When that location becomes available again, the actor is restored with its saved EMS state.
- Moving into an area that is not loaded is the one limit on travel. A low-frequency safety check captures such an actor, and it returns the same way.
- Normal EMS saves capture the actor at its current location.
- Gameplay destruction removes the persistent runtime record.

## NPCs and AI Controllers

Managed Pawns work like other World Partition Runtime Actors. When EMSAddons makes a managed Pawn dormant, the Pawn is destroyed and later respawned as a new actor.

AI Controllers remain normal transient Unreal actors. EMSAddons does not save or restore controller objects. When it removes a managed Pawn, it automatically destroys that Pawn's non-player Controller after normal unpossession, provided the Controller did not take possession of another Pawn. Player Controllers are never touched.

For an NPC:

1. Put persistent AI state on the Pawn and save it normally. If important state currently lives on the Controller or Blackboard, copy the values worth keeping onto the Pawn in **Actor Pre Save**.
2. Set **Auto Possess AI** to **Spawned** or **Placed in World or Spawned** so Unreal creates a Controller for the restored Pawn.
3. In **Actor Loaded**, apply any restored AI state that must be pushed back into the new Controller, Blackboard, perception setup, squad registration, or other runtime systems.

No controller persistence, special NPC component, or manual controller cleanup is required.

**Is Managed Actor Removal In Progress** remains useful when an actor has other external dependencies or when **End Play** must distinguish streaming removal from normal gameplay destruction.

Hard references to a dormant actor or its controller do not survive. Soft references by stable name do, which is what makes them the right way to point at a managed actor.

## When to use it

Use **World Partition Runtime Actors** when a runtime actor should behave like spatial World Partition content without requiring a placed owner.

Use normal Easy Multi Save when the actor can remain in the persistent level.

Use [Actor Spawner](ACTOR_SPAWNER.md) when the actor must explicitly belong to a particular placed spawner, streamed level, or cell.

## Important behavior

- Spawn locations must be covered by a currently visible supported generated cell.
- If a managed actor temporarily cannot resolve a supported cell, it stays alive and is retried.
- Spawning is rejected while a save or load is in progress, including the initial World Partition load. Gate spawns on **Is Saving Or Loading**, or wait for the EMS load to complete — an actor spawned into that window would be destroyed by the load that follows.
- Loading replaces the managed runtime actors with the ones the save holds. Managed actors spawned since that save was written are destroyed and do not return.
- The managed actor lives in the persistent level while active; EMSAddons supplies the World Partition streaming lifetime.
- Stable names help deterministic paths and soft references. Hard references do not automatically rebind after restoration.
- HLOD and Data-Layer-specific ownership are outside the supported scope.
- The system is server-authoritative. Replication remains the project's responsibility.
