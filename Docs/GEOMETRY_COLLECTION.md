# Geometry Collection Persistence

Use **EMS Geometry Collection Actor** to persist fractured Chaos Geometry Collection state through normal EMS saving.

## Basic use

1. Place an **EMS Geometry Collection Actor** instead of the standard Geometry Collection Actor.
2. Assign the Geometry Collection asset.
3. Configure Chaos settings normally.
4. Save and load through EMS normally.

The actor can live in streamed levels and World Partition cells like other placed actors.

## Blueprint nodes

- **Is EMS Addon Restore Complete** — checks whether deferred restoration has finished.
- **Get EMS Addon Restore Result** — returns the latest restore result.

Use the EMS Geometry Collection Actor as the target.

## Important behavior

- Saved data is expected to match the same Geometry Collection asset and hierarchy.
- Restoration may defer until the Chaos runtime and physics proxy are ready.
- Capture and restore are server-authoritative. Replication remains the project's responsibility.
