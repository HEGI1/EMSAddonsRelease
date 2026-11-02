# Geometry Collection Persistence

Use **EMS Geometry Collection Actor** to persist fractured Chaos Geometry Collection state through normal EMS saving.

## Basic use

1. Place an **EMS Geometry Collection Actor** instead of the standard Geometry Collection Actor.
2. Assign the Geometry Collection asset.
3. Configure Chaos settings normally.
4. Save and load through EMS normally.

The actor can live in streamed levels and World Partition cells like other placed actors. It carries an **EMS Physics Streaming Guard** so it holds position instead of falling out of the world while the geometry underneath it is streamed out; see [Physics Streaming Guard](PHYSICS_STREAMING_GUARD.md).

## Blueprint nodes

- **Is EMS Addon Restore Complete** — checks whether deferred restoration has finished.
- **Get EMS Addon Restore Result** — returns the latest restore result.

Use the EMS Geometry Collection Actor as the target.

## Important behavior

- By default, restoration requires the same Geometry Collection asset and hierarchy. **Asset Mismatch Policy → Allow If Hierarchy Matches** permits a different Rest Collection only when its hierarchy and authored transforms still match the saved state.
- Restoration may defer until the Chaos runtime and physics proxy are ready. A save during that short pending phase keeps the loaded Geometry state intact until restoration finishes.
- Restoration waits up to about five seconds of game time for those resources. If they never appear, the restore fails and the saved Geometry state is kept, not overwritten by the unrestored collection. It stays kept until the next restore, normally the next load.
- The addon persists fracture hierarchy and piece transforms. Chaos removal and decay lifecycle state is outside the persistence scope.
- Capture and restore are server-authoritative. Replication remains the project's responsibility.
