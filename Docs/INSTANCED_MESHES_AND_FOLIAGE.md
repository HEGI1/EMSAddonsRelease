# Instanced Meshes and Foliage

Use **EMS Instance Manager** to save gameplay changes to instanced meshes and painted foliage without creating a separate actor for every instance. Supported changes include adding, removing, moving, replacing, custom data, and tagged gameplay values.

## Basic use

1. Place **one EMS Instance Manager** in the persistent level.
2. Change supported instances during gameplay.
3. Save and load normally through EMS.

No registration is required. Add the `EMS.IgnoreInstances` tag to an actor or component to exclude its instances.

## Blueprint nodes

- **Get EMS Instanced Source Manager** — finds the active manager in the world.
- **Replace Instance** — swaps one instance for a different mesh.
- **Get Instance Gameplay Value** / **Set Instance Gameplay Value**
- **Remove Instance Gameplay Value** / **Clear Instance Gameplay Values**
- **Is EMS Addon Restore Complete** / **Get EMS Addon Restore Result**

Call the instance gameplay-value nodes on the active manager, using either a reference to the placed manager or **Get EMS Instanced Source Manager**.

## Replacing an instance

Use **Replace Instance** for permanent changes such as replacing a tree with a stump.

Pass the original **Component**, **Instance Index**, and **Replacement** settings. On success, use the returned **Replacement Component** and **Replacement Instance Index** for further changes. The original index may now refer to another instance.

Replacement options:

- **Mesh** — the replacement Static Mesh.
- **Transform Offset** — optional local position, rotation, and scale adjustment.
- **Preserve Scale** — keeps the original instance scale by default.

Custom data and tagged gameplay values are retained across a successful replacement. A no-op or invalid replacement returns false.

## Gameplay values

Use the gameplay-value nodes to save per-instance values such as health, harvested resources, or regrowth progress. Each value uses a Gameplay Tag and a float.

For hit results, use the hit component and instance index. After replacement, switch to the returned component and index. Values follow the replaced instance rather than its old location.

## Supported content

- ISM and HISM components on placed actors
- Streamed levels, Level Instances, and World Partition
- Painted Static Mesh Foliage

Not supported: ISM/HISM components on runtime-spawned actors, Actor Foliage, Landscape Grass, PCG/procedural output, GPU-only instances, or instance replication.

## Important behavior

- Use only one manager; it stays in the persistent level and is non-spatial.
- **Replace Instance** cannot run during an EMS save/load. It can also fail while affected streamed content is still restoring; retry once restoration completes.
- Instance gameplay-value writes may also need to wait until the affected source finishes restoring. Use the restore-status nodes to check overall progress.
- Exact overlapping instances cannot always retain distinct gameplay identities after reordering or editing. Avoid assigning unique gameplay state to indistinguishable duplicates.
- A source whose authored layout changes substantially between game versions may no longer match an older saved instance change.
- World Partition instance saves made before version **0.5.4** are incompatible with the current identity format; make a fresh save after upgrading.
- Persistence is server-authoritative. Replication is the project's responsibility.
