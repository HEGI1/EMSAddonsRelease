# World Partition Placed Actors

Use **EMS World Partition Placed Component** for editor-placed actors that should save their state and follow World Partition streaming as they move around the map.

## Basic use

1. Implement the **EMS Actor Save Interface** on the actor class.
2. Place the actor in the **main World Partition map**.
3. Add **EMS World Partition Placed Component** in the editor, either to the class or to the placed actor.
4. Save the map, then save and load normally through EMS.

Use **one component per actor**. Adding it at runtime does not enroll an actor.

## Blueprint nodes

Get **EMS World Partition Placed Subsystem** with **Get World Subsystem** to call:

- **Is Placed Actor Managed** — checks whether an actor is managed.
- **Get Managed Placed Actors** — returns currently active managed placed actors, not dormant records.

Call **Get Placement Id** on the component for the actor's persistent ID.

The shared **Is Managed Actor Removal In Progress** node can be used in **End Play** to distinguish automatic streaming removal from gameplay destruction. It works with both placed and runtime actors.

## Important behavior

- An actor stays active while its current location is covered by loaded World Partition content. When its area unloads, it is saved and removed, then recreated with its saved state when the area returns.
- Characters and simulating actors can also unload when streamed geometry supporting them disappears, even if another cell covers their location.
- Moving between cells needs no special handover. The actor retains its identity across streaming and saved-game loads.
- Gameplay destruction is persistent. Loading an earlier save can restore an actor that was alive in that save.
- If an older save has no data for the placed actor, it uses the actor's authored defaults.
- For relationships between streamed actors, prefer **soft references** or saved IDs. Hard actor references and attachments are not automatically rebound after recreation.
- Mark instance-specific variables for EMS saving when they must survive recreation. Unsaved instance overrides and additional instance components are not reconstructed.
- Author **EMS Physics Streaming Guard** on the actor class if it must remain available after recreation. See [Physics Streaming Guard](PHYSICS_STREAMING_GUARD.md).
- Up to **4,096** placed actor records are supported, including destroyed actors.
- Restoration is server-authoritative; replication is the project's responsibility.

## Limitations

Only main-world, persistent-level placements are supported. Runtime Data Layers, Level Instances, HLOD ownership, and conventional streamed sublevels are outside this feature's scope.

## Settings

**EMS World Partition Placed Component** has no settings of its own.

| Setting | Default | Meaning |
| --- | --- | --- |
| **Is Spatially Loaded** (on the actor) | Off, enforced | The component turns it off automatically. Leave it off; the addon streams the actor by its current location. |
