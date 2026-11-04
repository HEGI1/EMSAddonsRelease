# Geometry Collection Persistence

Use **EMS Geometry Collection Actor** to save and restore fractured Chaos Geometry Collection state with EMS.

## Basic use

1. Place an **EMS Geometry Collection Actor** instead of the standard Geometry Collection Actor.
2. Assign its Geometry Collection asset and configure Chaos normally.
3. Save and load through EMS.

The actor supports streamed levels and World Partition. It already includes **EMS Physics Streaming Guard**; see [Physics Streaming Guard](PHYSICS_STREAMING_GUARD.md).

## Blueprint nodes

- **Is EMS Addon Restore Complete** — checks whether restoration has finished.
- **Get EMS Addon Restore Result** — checks its outcome.

Call these nodes on the EMS Geometry Collection Actor.

## Important behavior

- By default, restoration requires the same Geometry Collection asset and hierarchy. **Asset Mismatch Policy → Allow If Hierarchy Matches** allows a different asset only when the hierarchy and authored transforms match.
- Restoration can wait briefly for Chaos to become ready. If it cannot finish, the saved state is retained for a later load.
- Fracture hierarchy and piece transforms are saved; Chaos removal and decay state are not.
- Capture and restoration are server-authoritative. The project handles replication.
