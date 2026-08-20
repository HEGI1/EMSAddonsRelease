# Instanced Meshes and Foliage

Use this addon to persist runtime changes to instanced static meshes without turning every instance into a saved actor.

The **EMS Instance Manager** handles both project-owned ISM and HISM components and painted Static Mesh Foliage. It saves changes relative to the authored layout, including added, removed, moved, replaced, custom-data, and per-instance gameplay state.

## Basic use

1. Place one **EMS Instance Manager** in the persistent level.
2. Modify instances normally during gameplay.
3. Save and load through EMS normally.

One manager covers every supported source in the world. Painted foliage is discovered through the Instanced Foliage Actor and ordinary instances through their owning actor, but both end up in the same manager and the same save.

Nothing needs to be registered manually. Add `EMS.IgnoreInstances` to an actor or component when its instances should be ignored.

## Blueprint nodes

- **Get EMS Instanced Source Manager** — gets the active Instance Manager.
- **Replace Instance** — replaces one ISM, HISM, or painted foliage instance using compact replacement settings.
- **Get Instance Gameplay Value** — reads one tagged float from an instance.
- **Set Instance Gameplay Value** — writes one tagged float to an instance.
- **Remove Instance Gameplay Value** — removes one tagged value.
- **Clear Instance Gameplay Values** — removes all gameplay values from an instance.
- **Is EMS Addon Restore Complete**
- **Get EMS Addon Restore Result**

The complete gameplay-data container is internal. Blueprint only works with individual tagged values.

## Replacing an instance

Use **Replace Instance** for permanent mesh changes such as an intact tree becoming a stump or a rock becoming a mined variant.

Inputs are:

- the hit **Component**
- the hit **Instance Index**
- **Replacement** settings

Outputs are:

- **Replacement Component** — the component that now owns the replacement.
- **Replacement Instance Index** — the replacement's current instance index.

The node has **Success** and **Failed** execution outputs. **Success means an actual replacement completed**: the new instance was created and the source instance was removed. A default replacement using the same mesh is a no-op and goes through **Failed** instead of pretending a replacement happened.

Use the replacement outputs from **Success** for operations that happen after replacement, such as **Set Instance Gameplay Value**. The original hit Component and Instance Index identify the removed source instance; on a source with many instances that old index may immediately belong to a different instance after removal.

**Replacement** contains only:

- **Mesh** — Static Mesh used by the replacement.
- **Transform Offset** — optional local translation, rotation, and scale adjustment.
- **Preserve Scale** — enabled by default, so the replacement keeps the original per-instance scale, including foliage random scale.

With the defaults, replacement behavior is unchanged: the complete original instance transform is preserved. Disable **Preserve Scale** to reset the base scale to `1,1,1` before **Transform Offset** is applied. This is useful when the replacement mesh has different authored dimensions.

The manager preserves per-instance custom data and EMS gameplay values automatically, then records the result through the normal sparse instance delta. Gameplay values assigned before replacement are carried across automatically; gameplay values assigned afterward should use the replacement outputs from the **Success** path.

If a compatible target source already exists, it is reused. Otherwise the manager creates a replacement source from the original source settings and remembers how to recreate it for later streaming or load operations.

For painted Static Mesh Foliage the authored Foliage Type is never changed. The replacement is routed to a target the manager owns on the Instanced Foliage Actor, so changing one tree does not change every tree that uses the same Foliage Type. The replaced instance leaves the foliage system and is persisted like any other managed instance; foliage painting tools no longer account for it.

## Supported sources

- Placed actors with ISM or HISM components
- Streamed levels and Level Instances
- World Partition cells
- Painted Static Mesh Foliage

Runtime-spawned actors that own ISM/HISM components are not handled because they do not provide stable authored source identity between sessions.

Actor Foliage, Landscape Grass, PCG output, procedural regeneration, GPU-only instances, and instance replication are outside the supported scope.

## Per-instance gameplay values

Use the gameplay-value nodes for persistent values such as health, harvest progress, resource state, or regrowth timers. Values are addressed by Gameplay Tag and stored separately from rendering custom data.

For a normal instance, a line trace can pass its hit component and hit item directly to **Get Instance Gameplay Value** or **Set Instance Gameplay Value**. After **Replace Instance**, use the returned Replacement Component and Replacement Instance Index from the **Success** path instead of reusing the original hit pair.

## Permanent state changes

For permanent world changes, prefer **Replace Instance** over spawning long-lived gameplay actors. The manager internally expresses the replacement as ordinary source removal plus target-source addition, so it uses the same save and restore path as every other instance change.

Temporary actors remain useful for active behavior such as physics, falling trees, damage reactions, or debris. Once the temporary behavior ends, the final static state can be represented by the replacement instance again.

## Important behavior

- The manager must remain in the persistent level and automatically disables **Is Spatially Loaded**. Placing more than one is refused with an error.
- Moving an instance changes its persistent identity because transform participates in matching.
- Exact overlapping duplicates are supported, but should not be treated as individually meaningful after arbitrary external reordering.
- If a source cannot be captured, its previous saved delta is retained.
- Capture, replacement, and restore are server-authoritative. EMSAddons does not replicate instance changes.

Use [Actor Spawner](ACTOR_SPAWNER.md) only when a runtime actor must also inherit explicit streamed-level or cell ownership.
