# Geometry Collection

Use **EMS Geometry Collection Actor** to save which pieces of a Chaos Geometry Collection have broken off and where they are.

## Quick start

1. Place an **EMS Geometry Collection Actor** instead of the standard Geometry Collection Actor.
2. Assign its Geometry Collection asset and set up Chaos as usual.
3. Save and load normally through EMS.

It works in streamed levels and World Partition, and already includes a [Physics Streaming Guard](PHYSICS_STREAMING_GUARD.md).

## Good to know

- Fracture state and piece positions are saved. Chaos removal and decay timers are not.
- By default, a save only restores onto the same Geometry Collection asset. See **Asset Mismatch Policy** below.
- Restoration may take a moment while Chaos starts up. If it can't finish, the saved state is kept for the next load.
- Saving and restoration run on the server or in standalone. Replication is your project's responsibility.

## Blueprint nodes

| Task | Nodes |
| --- | --- |
| Check whether restoration finished, and how | **Is EMS Addon Restore Complete**, **Get EMS Addon Restore Result** |

Use the actor as **Target**. "Complete" doesn't always mean "succeeded"; check the result.

## Settings

| Setting | Default | Purpose |
| --- | --- | --- |
| **Asset Mismatch Policy** | Reject | **Reject** restores only onto the saved asset. **Allow If Hierarchy Matches** also accepts a different asset with the same pieces. |
