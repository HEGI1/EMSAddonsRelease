# Changelog

## Unreleased

### Added

- **Instanced Meshes and Foliage:** **Replace Instance** replaces one managed ISM, HISM, or painted Static Mesh Foliage instance using compact replacement settings for the mesh, optional local transform offset, and scale preservation. The manager routes the replacement internally, preserves custom data and per-instance gameplay values, and recreates generated replacement sources for later streaming and load operations. Authored Foliage Types are never modified: a replaced foliage instance moves to a target the manager owns on the Instanced Foliage Actor.
- **World Partition Runtime Actors:** **Get World Partition Runtime Actors** returns the currently live managed actors, not just their count.
- **World Partition Runtime Actors:** **Is Managed Actor Removal In Progress** lets a managed actor tell, from **End Play**, that EMSAddons is removing it rather than gameplay destroying it. Documented alongside a pattern for NPCs that own an AI Controller.
- **Autosave and Checkpoints:** **Autosave When Leaving Map** optionally saves the current EMS Player/Level data synchronously immediately before normal map travel. It reuses EMS's native save preparation and Multi-Level merge path, ignores the minimum autosave interval, and skips safely during checkpoint travel, active EMS tasks, blockers, or unsafe streaming.

### Improved

- **Instanced Meshes and Foliage:** streamlined the Blueprint gameplay-state API to tagged value operations. The complete per-instance gameplay-data container remains internal to C++ and persistence.
- **World Partition Runtime Actors:** managed Pawns now clean up their orphaned non-player Controller automatically when EMSAddons removes them. Restored Pawns continue to use Unreal's native **Auto Possess AI** behavior; no controller persistence or extra NPC system is added.
- **Instanced Meshes and Foliage:** merged the separate **EMS Foliage Manager** into **EMS Instance Manager**. One manager now handles ISM, HISM, and painted Static Mesh Foliage, reducing setup to a single persistent manager. Existing development levels using an **EMS Foliage Manager** must replace it with an **EMS Instance Manager**.

### Fixed

- **Actor Spawner:** made restore safe when actor load callbacks spawn through the same spawner and modify its manifest.
- **World Partition Runtime Actors:** pre-save capture now runs on the game thread when EMS Multi-Thread Saving is enabled.
- **World Partition Runtime Actors:** **Spawn World Partition Runtime Actor** no longer rejects every spawn in a partitioned world. The cell query used a zero radius, which matches no cells at all, so every location appeared to have no visible cell.
- **World Partition Runtime Actors:** managed actors no longer disappear while the world around them stays loaded. Cell ownership is now treated as coverage — an actor stays live for as long as any visible supported cell covers its location — instead of binding it to one chosen cell whose visibility could change independently.
- **World Partition Runtime Actors:** spawning while a save or load is in progress, including the initial World Partition load, is now rejected with a warning instead of producing an actor that the load immediately destroys.
- **World Partition Runtime Actors:** a load that destroys live managed actors now says so in the log.
- **Autosave and Checkpoints:** **EMS Checkpoint** now implements EMS's Actor Save Interface, so it participates in normal (non-checkpoint) Save/Load Game Actors like any other addon actor. Previously it did not, so a **Trigger Once** checkpoint's already-activated state existed only in memory: a later unrelated save superseded the one stored checkpoint record, and reloading let the player trigger the same checkpoint again.

## 0.2.0 - 2026-08-11

### Added

- **Autosave and Checkpoints** with configurable EMS save content, periodic autosaves, post-map-load autosaves, blockers, checkpoint activation, and cross-map checkpoint loading.
- **World Partition Runtime Actors** with **Spawn World Partition Runtime Actor**. Managed actors follow World Partition streaming from their current location without requiring a placed manager.
- Shared restore-status Blueprint nodes: **Is EMS Addon Restore Complete** and **Get EMS Addon Restore Result**.
- Persistent per-instance gameplay data for ISM/HISM and foliage instances.
- `EMS.IgnoreInstances` opt-out tag for actors and components.

### Improved

- **Actor Spawner** now uses one placeable spawner for runtime actors that must inherit exact streamed-level or World Partition cell ownership. Spawn, capture, removal, and restore are transactional.
- **Instanced Meshes and Foliage** now share one persistence path for project-owned ISM/HISM components and painted Static Mesh Foliage.
- **Level Sequence** restore now treats playback position, play rate, direction, and finished state as one coherent saved state.
- Addon persistence uses one server-authoritative rule and marshals save-time actor/component access to the game thread where required.
- Placeable workflow actors include editor-only billboard sprites and the Blueprint surface was reduced to workflow-relevant nodes.

### Fixed

- Streamed hide/show no longer leaves Geometry Collection, Level Sequence, or Instance/Foliage managers unable to capture or restore.
- Autosave retries no longer recurse when an EMS async save task cannot start.
- Checkpoint startup/load overlap no longer causes an immediate unintended save.
- **Load Last Checkpoint** restores the player to the checkpoint trigger center, including when the checkpoint streams in after the EMS load completes.
- Loading cancels pending addon autosaves so restored state is not immediately overwritten.
- Repeatable checkpoints create a fresh checkpoint save on genuine re-entry.
- Slot/user changes correctly cancel stale checkpoint work.
- Instanced mesh and foliage capture/restore now consistently follow server authority.
- Actor Spawner no longer keeps references into its manifest across gameplay callbacks that can modify it.
- **Destroy Spawned Actor** retains the persistent record when the live actor cannot actually be destroyed.
- Level Sequence state now serializes independently of EMS **Auto Save Structs** and authored autoplay no longer races or stalls during restore.
- Client-side addon restores report **Skipped** when persistence authority is intentionally absent.

### Supported scope

- Checkpoints use the current EMS save slot; a later normal **Save Game Actors** operation supersedes the current checkpoint.
- Instance changes are persistent but are not replicated by EMSAddons.
- Painted Static Mesh Foliage is supported. Actor Foliage, Landscape Grass, PCG output, procedural regeneration, GPU-only instances, and instance replication are outside the supported scope.
- World Partition Runtime Actors do not currently provide HLOD or Data-Layer-specific ownership.

## 0.1.0 - 2026-07-29

- Added the initial EMSAddons plugin modules and experimental persistence workflows for runtime actors, Geometry Collections, Level Sequences, ISM/HISM components, and painted foliage.
