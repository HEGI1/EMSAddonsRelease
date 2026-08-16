# Instanced Meshes and Foliage

Use this addon to persist runtime changes to instanced static meshes without turning every instance into a saved actor.

The **EMS Instance Manager** handles both project-owned ISM and HISM components and painted Static Mesh Foliage. It saves changes relative to the authored layout, including added, removed, moved, custom-data, and per-instance gameplay state.

## Basic use

1. Place one **EMS Instance Manager** in the persistent level.
2. Modify instances normally during gameplay.
3. Save and load through EMS normally.

One manager covers every supported source in the world. Painted foliage is discovered through the Instanced Foliage Actor and ordinary instances through their owning actor, but both end up in the same manager and the same save.

Nothing needs to be registered manually. Add `EMS.IgnoreInstances` to an actor or component when its instances should be ignored.

## Blueprint nodes

- **Get EMS Instanced Source Manager** — gets the active Instance Manager.
- **Get Instance Gameplay Value**
- **Set Instance Gameplay Value**
- **Get Instance Gameplay Data**
- **Set Instance Gameplay Data**
- **Remove Instance Gameplay Value**
- **Clear Instance Gameplay Data**
- **Is EMS Addon Restore Complete**
- **Get EMS Addon Restore Result**

Adding, removing, or moving instances still uses Unreal's normal ISM/HISM or foliage functions. EMSAddons saves the resulting state.

## Supported sources

- Placed actors with ISM or HISM components
- Streamed levels and Level Instances
- World Partition cells
- Painted Static Mesh Foliage

Runtime-spawned actors that own ISM/HISM components are not handled because they do not provide stable authored source identity between sessions.

Actor Foliage, Landscape Grass, PCG output, procedural regeneration, GPU-only instances, and instance replication are outside the supported scope.

## Per-instance gameplay data

Use the gameplay-data nodes for persistent values such as health, harvest progress, resource state, or regrowth timers. This data is stored separately from rendering custom data.

## Permanent state changes

For permanent world changes, prefer instance edits over long-lived gameplay actors. For example, remove an intact tree instance and add a stump instance. Both changes are saved by the same manager.

Temporary actors remain useful for active behavior such as physics, falling trees, damage reactions, or debris.

## Important behavior

- The manager must remain in the persistent level and automatically disables **Is Spatially Loaded**. Placing more than one is refused with an error.
- Moving an instance changes its persistent identity because transform participates in matching.
- Exact overlapping duplicates are supported, but should not be treated as individually meaningful after arbitrary external reordering.
- If a source cannot be captured, its previous saved delta is retained.
- Capture and restore are server-authoritative. EMSAddons does not replicate instance changes.

Use [Actor Spawner](ACTOR_SPAWNER.md) only when a runtime actor must also inherit explicit streamed-level or cell ownership.
