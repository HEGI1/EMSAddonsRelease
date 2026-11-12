# Instanced Meshes and Foliage

Use **EMS Instance Manager** to save gameplay changes to instanced meshes and painted foliage, such as chopped trees, mined rocks, or moved props, without turning each instance into an actor.

## Quick start

1. Place **one EMS Instance Manager** in the persistent level.
2. Change instances during gameplay: add, remove, move, set custom data, or use the nodes below.
3. Save and load normally through EMS.

Instances are found automatically. To exclude some, add the `EMS.IgnoreInstances` tag to the actor or component.

## Supported content

- ISM and HISM components on placed actors, including in streamed levels, Level Instances, and World Partition
- Painted Static Mesh Foliage

Not supported: instances on runtime-spawned actors, Actor Foliage, Landscape Grass, PCG or procedural output, GPU-only instances, and replication.

## Replace an instance

Use **Replace Instance** for permanent swaps, such as a tree becoming a stump. Pass the **Component**, **Instance Index**, and the replacement **Mesh**, with an optional **Transform Offset** and **Preserve Scale** (on by default).

On success, use the returned **Replacement Component** and **Replacement Instance Index** from then on; the old index may now point to a different instance. Custom data and gameplay values move with the replacement.

## Gameplay values

Store per-instance numbers, such as health or regrowth progress, with **Set Instance Gameplay Value**. Each value is a Gameplay Tag plus a float. From a hit result, pass the hit component and instance index.

## Good to know

- Use one manager per world. It stays in the persistent level.
- **Replace Instance** and gameplay-value writes can fail while EMS is saving or loading, or while the affected area is still restoring. Try again once restoration finishes.
- Avoid giving unique gameplay values to instances that sit exactly on top of each other; they can't always be told apart.
- If you substantially rearrange instances in a later game version, older saved changes to them may no longer apply.
- Limits per save: 4,096 changed components or foliage types, and 500,000 changes. Each addition, removal, custom-data change, and gameplay-value change counts once; a move counts twice.
- Saving runs on the server or in standalone. Replication is your project's responsibility.

## Blueprint nodes

Call these on **EMS Instance Manager**. To find it, use **Get EMS Instanced Source Manager** with **Manager Class** set to **EMS Instance Manager**.

| Task | Nodes |
| --- | --- |
| Swap an instance's mesh | **Replace Instance** |
| Read or write a value | **Get Instance Gameplay Value**, **Set Instance Gameplay Value** |
| Remove values | **Remove Instance Gameplay Value**, **Clear Instance Gameplay Values** |
| Check whether restoration finished, and how | **Is EMS Addon Restore Complete**, **Get EMS Addon Restore Result** |

## Tags

The manager has no settings. Use tags instead:

| Tag | Purpose |
| --- | --- |
| `EMS.IgnoreInstances` | Add to an actor or component to exclude its instances. |
| `EMSGeneratedReplacement` | Added by the addon to components created by **Replace Instance**. Don't add or remove it. |
