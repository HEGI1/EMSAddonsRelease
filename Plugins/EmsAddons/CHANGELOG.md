# Changelog

## 1.3.2 - 2026-11-12

### Documentation

- Streamlined all addon guides around a quick start and a short "Good to know" list, with details kept out of the way.
- **World Partition Placed and Runtime Actors:** EMS **Basic** and **Full** multi-level saving both keep several World Partition maps in one slot. The guides previously named only Full.
- **README:** EMS 1.86 or newer is required for Autosave and Checkpoints with World Partition or level streaming. The plugin to enable is listed as **Easy Multi Save Addons**.

## 1.3.0 - 2026-11-12

### Fixed

- **World Partition runtime and placed actors:** save the shared manager per map instead of as an EMS traveling Persistent Actor. Basic/Full saves keep maps separate, including when saving an ordinary map. Map ownership checks reject foreign records and respect EMS level redirects.

### Compatibility

- **Existing World Partition saves:** records without map ownership are rejected with a warning, including old single-map saves. Managed runtime actors are not restored; placed actors return to authored defaults, including previously destroyed placements. Save again to write map-scoped state.

## 1.2.8 - 2026-11-12

### Fixed

- **Autosave and Checkpoints:** a World Partition cell or sub-level streaming in is no longer treated like a game load. During those frames a checkpoint entered by the player was ignored until they left and re-entered it, **Request Autosave** (including the periodic trigger) returned false, and a request waiting for the minimum interval or a blocker was canceled without an event. The save now waits for the streaming load and then runs. A real EMS game load still refuses and cancels as before.
- **Autosave and Checkpoints:** **Manual Save** refuses a **Save Name** of which EMS stores no character, such as `??`. Its slot was `<profile>_manual_`, which is outside the profile: once current, **Get Save Slots** listed only that slot, and with **Autosave Slot Count** 0 later autosaves kept writing into it. A **Checkpoint Slot Name** like that now selects the default checkpoint slot.
- **Autosave rotation:** with **Autosave Slot Count** 3 or more, a slot known to be incomplete is replaced before any complete save. A canceled or cut-off save could leave it with the newest time, so two complete saves were overwritten first.
- **Load Last Checkpoint:** when EMS fails to load a checkpoint on the current map, the slot of the progress still running becomes current again, as it already did when the load could not start. Previously the checkpoint slot stayed current and later saves carried its custom files.
- **Failed saves:** when the addon cannot complete a save that EMS reported as successful and EMS also refuses the switch back, the recovery journal now survives EMS's own success event, so the next start returns to the last complete save. That event also no longer counts as a successful save for the minimum interval.

## 1.2.7 - 2026-11-11

### Added

- **Autosave and Checkpoints:** **Excluded Maps** setting for the main menu, front-end and loading maps. The addon never saves on them: **Request Autosave**, **Manual Save** and checkpoint activation are refused, periodic and after-load autosaves are not armed, and **Autosave When Leaving Map** skips travel away from them. **Load Save** and **Load Last Checkpoint** still work there.

### Fixed

- **Autosave preparation:** persist user-scoped target exclusions before checkpoint/custom-file writes in async and synchronous map-leave saves. Canceled starts, partial carried-file copies and failed cleanup can no longer expose mixed snapshots through addon menus or loads after returning to the previous slot or restarting. Complete addon replacements restore availability.
- **Running saves:** the target exclusion no longer applies while its save is still running. **Get Save Slots**, **Get Save Slot Details**, **Has Checkpoint** and **Get Current Checkpoint** keep reporting the slot being written and its carried checkpoint; previously every autosave briefly hid them, including the only slot with **Autosave Slot Count** 0. Failed, canceled and interrupted targets stay excluded as before.
- **Pause menus:** ordinary Level-only disk saves are treated as conflicts and leave the game paused. Only identifiable streaming loads are waited out; EMS 1.85/1.86 keep the flags identifying memory-only streaming saves private, so those saves also require unpausing first.
- **Persistence edge cases:** generated instance replacements remain reconstructible after loading older saves; checkpoint references reject overwritten activations; placed save callbacks commit runtime actor deaths before serialization; held physics objects resume after movement onto loaded ground; deferred pause-menu operations stay bound to their original EMS user and slot and report failure once.
- **Autosave and Checkpoints:** automatic autosaves could save a main menu or loading map into the last played profile, because EMS keeps that slot current. With **Autosave Slot Count** 0 this replaced the player's progress, and **Load Save** or Continue then opened the menu. List such maps in **Excluded Maps**.
- **Autosave and Checkpoints:** **Manual Save**, **Load Save** and a map-changing **Load Last Checkpoint** no longer fail from a pause menu when the pause stopped an EMS streaming load mid-task (World Partition or level streaming). The request is accepted; the game runs until that work finishes (at most 2 seconds), pauses again and the request runs.

### Documentation

- **Autosave and Checkpoints:** autosave blockers are cleared when the map changes, including blockers added from the Game Instance.

## 1.2.6 - 2026-11-10

### Documentation

- **Autosave and Checkpoints:** shortened the display-name reference, grouped failure/recovery rules, and clarified incomplete-slot filtering, single-slot reuse and EMS 1.85/1.86 streaming behavior.

### Fixed

- **Autosave and Checkpoints:** pending requests and save-start revalidation are bound to the EMS user; cleanup cannot roll back or delete another user's same-named slot.
- **Autosave failure handling:** verifies fresh Slot Info and removal of stale streaming Level data, preflights carried source files, and excludes incomplete targets from addon menus and loads when preparation or cleanup fails. A successful replacement clears the session exclusion.
- **Checkpoint and crash recovery:** keeps the save journal through checkpoint-record persistence, discards targets whose record failed, retains recovery journals when switching back is refused, and reports deletion failures accurately. Recovery deletion remains best effort across process restarts.

## 1.2.5 - 2026-11-10

### Documentation

- **Autosave and Checkpoints:** describes that manual save names and **Checkpoint Slot Name** values differing only in case share one slot, and that **Get Save Profile** returns the lowercase profile after a save to a generated slot.

## 1.2.4 - 2026-11-10

### Changed

- **Autosave and Checkpoints:** generated autosave, manual, and checkpoint slot IDs are lowercase. Existing mixed-case slots are reused with their original paths; display-name capitalization is preserved.

### Added

- **Autosave and Checkpoints:** persistent display names independent of EMS slot identifiers. Checkpoints use their **Display Name**; **Manual Save** and **Request Autosave** accept an optional **Display Name**. **Set Autosave Display Name** supplies the location label for future automatic saves.
- **Get Save Slots With Details**, **Get Save Slot Details**, and **Set Save Slot Display Name** support save menus and label-only renaming. Renaming preserves timestamps and rotation order. Existing saves receive fallback labels without migration; duplicate aliases are allowed.

### Maintenance

- **Release checks:** publisher integration tests follow the current plugin version instead of assuming a fixed release number. Version metadata continues to be applied only when publishing.

## 1.2.3 - 2026-11-09

### Documentation

- **Autosave and Checkpoints:** describes that a save without Level in **Save Data Flags** leaves the replaced slot without Level data under streaming, and that a save whose switch back EMS refuses is recovered on the next start.

## 1.2.2 - 2026-11-09

### Fixed

- **Autosave and Checkpoints:** when an **On Autosave Started** callback starts another EMS operation, or a map-leave save is aborted, and EMS refuses the switch back to the previous slot, the interrupted-save record is kept, so the next start still returns to the previous slot.

### Notes

- **Autosave and Checkpoints:** with World Partition or level streaming, a save whose **Save Data Flags** do not include Level also removes the replaced slot's old Level data. Keeping it would make streamed cells reload that older state into the running world. The slot has no Level data until the game saves Level data to it again.

## 1.2.1 - 2026-11-07

### Fixed

- **Autosave and Checkpoints:** works with the final Easy Multi Save 1.86, which changed how slot switches treat streaming. With World Partition or level streaming, a save that replaces an existing slot first deletes that slot's old Level data, so streamed cells keep restoring from memory after the save. Until the save finishes, that slot cannot be loaded.

## 1.2.0 - 2026-11-07

### Added

- **Autosave and Checkpoints:** save slots. Each save is an ordinary EMS slot of the current profile and becomes the current slot, so a normal EMS load continues from the newest save.
  - **Autosave Slot Count** rotates autosaves through `<Profile>_Autosave_0` to `<Profile>_Autosave_<N-1>`. Zero (default) keeps 1.1 behavior.
  - **Manual Save** writes `<Profile>_Manual_<Name>`.
  - **Load Save**, **Get Save Slots**, **Get Save Profile**, **Get Slot Profile**, **On Save Loaded** and **On Save Load Failed** load and list saves.
  - **Carried Custom Files** and **Add Carried Custom File** carry slot-bound custom saves into each new save.
  - After an interrupted save to a different slot, the next start can return to the previous existing slot and delete the incomplete slot. A later complete save of that slot, including the game's own **Save Game Actors**, keeps it.
  - When EMS reports that a save to a different slot failed, the addon returns to the previous slot and deletes the incomplete slot, so **Get Save Slots** never lists it and **Load Save** never loads it.
  - Requires EMS 1.86 for World Partition and level streaming. With EMS 1.85 the addon logs a warning, because switching slots there clears memory loading.

### Changed

- **Autosave and Checkpoints:** new checkpoints save into their own slot, `<Profile>_Checkpoint` or `<Profile>_Checkpoint_<Checkpoint Slot Name>`. Later addon saves retain a reference to the checkpoint. Loadable 1.1 checkpoints remain in their original slots until those slots are overwritten.

### Fixed

- **Autosave and Checkpoints:** in Play In Editor, a checkpoint read back from a save no longer fails to match its map, so **Load Last Checkpoint** loads it instead of travelling to the wrong world.
- **Autosave and Checkpoints:** checkpoint triggers are re-armed after a save is loaded.
- **Autosave and Checkpoints:** when **Load Last Checkpoint** cannot open the checkpoint's map, it reports **On Checkpoint Load Failed** and returns to the previous slot. Previously the load stayed pending and blocked later autosaves.
- **Autosave and Checkpoints:** while the game is paused, **Load Last Checkpoint** for a checkpoint on the current map returns false and changes nothing, because EMS does not load during pause. Unpause first. Checkpoints on another map still load, since opening the map ends the pause.
- **Physics:** the streaming guard no longer holds a body spawned over a real void once the world below has loaded (World Partition, or maps whose streamed sublevels are all visible); the body falls instead of hanging in the air.
- **Physics:** the streaming guard checks the whole footprint of each simulating body, so wide or overhanging bodies and broken Geometry Collections are held when the floor they rest on streams out.

## 1.1.2 - 2026-11-04

### Added

- **World Partition Placed Actors:** save and restore editor-placed actors as they move between World Partition cells. Add **EMS World Partition Placed Component** to an eligible placed actor; its non-spatial setting is enforced automatically.

### Changed

- **Actor Spawner:** ordinary gameplay spawns are blocked while EMS is busy, with restoration callbacks retaining their supported reentrant behavior.
- **Instanced Meshes and Foliage:** **Replace Instance** now refuses changes during an EMS save or load. Retry once the operation completes.

### Improved

- **World Partition:** more reliable restoration when an actor's supporting streamed geometry disappears, including overlapping cells.
- **Physics Streaming Guard:** preserves intended gravity when used with World Partition Placed Actors.
- **Managed actors and instances:** more reliable persistence during asynchronous saves, loads, and streaming transitions.

### Fixed

- **World Partition Placed Actors:** gameplay destruction and earlier saves now restore the correct state across streaming and map reloads.
- **World Partition Runtime Actors:** actors wait for their supporting streamed geometry before returning after load.
- **Actor Spawner and World Partition:** managed actor removals triggered by restoration callbacks are saved correctly.

## 1.1.0 - 2026-11-02

### Improved

- **Instanced Meshes and Foliage:** streamlined streaming discovery and replacement reconstruction to process arriving levels instead of rescanning the world. Unloaded sources release unused caches while modified baselines remain available for restoration.
- **World Partition Runtime Actors:** improved dormant-actor recovery when generated cells change, reduced repeated work for unrestorable records, and hardened automatic manager and actor ownership cleanup.
- **Level Sequence:** refined loop tracking for reverse playback and multiple wraps in one update. Restored runs honor their remaining repeats without changing the authored loop count for later playback.
- **Autosave and Checkpoints:** checkpoints can activate for other players while the EMS-configured pawn is unavailable, while still suppressing startup overlaps and placing that pawn safely when it appears.

### Fixed

- **Instanced Meshes and Foliage:** preserved saved deltas across streaming arrival and next-tick reconciliation. Conflicting gameplay writes or replacements wait for affected sources and targets, and restore-status queries report `Pending` while queued work remains.
- **Instanced Meshes and Foliage:** corrected gameplay-value association after external transform edits, duplicate-key changes, and instance replacement or regrowth. Incompatible saved deltas produce a warning when a later capture uses live state; unusually large instance scales no longer overflow identity keys.
- **Actor Spawner and World Partition Runtime Actors:** restored actors reclaim stable names even before destroyed predecessors are garbage-collected. Actor Spawner also retains actors created by restore callbacks.
- **World Partition Runtime Actors:** support detection includes level-owned collision, including BSP and character movement-base components without an actor owner, so actors are captured before their supporting cell disappears.
- **Physics Streaming Guard:** recognizes BSP as support at startup and during level streaming, and ignores collision from departing levels when finding remaining support.
- **Autosave and Checkpoints:** corrected paused-state restoration after canceled saves, checkpoint failure notifications, and clearing of superseded checkpoints during map-leave saves. Checkpoint activation supports any player's pawn and restores the EMS-selected pawn.
- **Checkpoint and Actor Spawner identities:** **Move to Level** preserves persistent IDs; **Copy Actors to Level** and direct editor text imports generate new IDs.
- **Level Sequence:** preserves playback direction for stopped and finished sequences without starting playback, and retains saved state when EMS or binding waits expire instead of reverting to autoplay.
- **Geometry Collection:** restoration allows approximately five seconds of game time for Chaos readiness and retains saved data when restoration cannot complete.

## 1.0.0 - 2026-09-20

### Added

- Initial stable 1.0 release.

## 0.5.9 - 2026-09-20

### Fixed

- **Actor Spawner:** spawning from a child's save callback now returns no actor or ID while capture is running, preventing incomplete child records from being written into the save. Retry the spawn after saving completes.
- **Level Sequence:** pausing and resuming now preserves the requested runtime loop count and completed-loop progress, including paused playback restored from a save.
- **Level Sequence:** actors restored through Actor Spawner or World Partition Runtime Actors now finish deferred playback restoration after EMS loading becomes idle, without waiting for an EMS loaded-actor list that excludes managed children.
- **Autosave and Checkpoints:** replacing a queued checkpoint now updates the queue before firing failure callbacks, so a listener that retries the canceled checkpoint cannot leave it permanently activation-pending without a queued save.
- **Autosave and Checkpoints:** autosave callbacks can no longer start overlapping addon EMS saves. Normal async saves keep a stable task across the start callback, while map-leave saves keep their synchronous reservation through addon and EMS completion callbacks and revalidate the outgoing save state before writing.
- **World Partition Runtime Actors:** unloading the last visible generated cell now retains the departing cell as coverage evidence while excluding it from visibility, so the actor becomes dormant immediately instead of normally waiting for the one-second reconciliation fallback.
- **Instanced Meshes and Foliage:** capture now applies the 500,000-change safety limit to the final merged delta set, including retained streamed-out or skipped sources, and preserves the complete previous saved delta set when the merged limit is exceeded.
- **Level Sequence:** a save taken while playback restoration is waiting for EMS or bindings now preserves the loaded state instead of capturing the temporarily stopped player.
- **Instanced Meshes and Foliage:** replacement requests with invalid transform offsets or invalid composed transforms are rejected before creating a replacement or removing the original instance.

## 0.5.4 - 2026-08-30

### Improved

- **Instanced Meshes and Foliage:** ISM/HISM/foliage sources in a World Partition world now identify their owning actor by Unreal's stable actor-instance GUID instead of an object path into the runtime cell's transient package. Streaming and reloading a cell no longer risks matching a saved delta to the wrong actor or losing it entirely.
- **Instanced Meshes and Foliage:** a streaming level's instances are now captured just before Unreal tears its components down, instead of after, and restored only once the level has both arrived and finished registering its components. This removes a whole-world foliage rescan on every streaming event and a source of missed captures on World Partition cell unload.
- **Actor Spawner:** a restored actor now runs **Actor Pre Load** before its Construction Script and Begin Play, matching how EMS Core respawns its own runtime actors.
- **World Partition Runtime Actors:** a restored managed actor now runs **Actor Pre Load** before its Construction Script and Begin Play, matching EMS Core.

### Fixed

- **Instanced Meshes and Foliage:** World Partition instance saves made before this release are no longer valid and will not restore. Re-place or re-capture affected sources after updating.
- **Instanced Meshes and Foliage:** a replacement created while another World Partition cell finishes streaming no longer gets reconciled away as an unrestored source.
- **Actor Spawner:** a restored actor whose saved state fails to load is discarded instead of being left with **Actor Pre Load** raised and **Actor Loaded** never delivered. Its record is kept for the next restore.
- **Autosave and Checkpoints:** a Trigger Once checkpoint rebuilt by a respawn or a streamed-level reload no longer reports a second activation when its committed checkpoint already matches. **On Checkpoint Activated** now fires only for accepted activations.
- **Instanced Meshes and Foliage:** hiding and showing a World Partition cell no longer reclassifies its live replaced instances as authored baseline content. Changed sources retain their original baseline until the manager leaves the world.
- **Physics Streaming Guard:** the Begin Play missing-world check no longer runs in a static world with no World Partition streaming and no streamed sublevels, so a simulating actor placed over empty space is not frozen with gravity off forever. World Partition and conventional streamed-sublevel worlds keep the check.
- **Actor Spawner:** a restored actor that fails its state load, or is no longer the actor bound to its record afterward, is no longer counted as restored.
- **Autosave and Checkpoints:** activating a checkpoint no longer broadcasts **On Checkpoint Activated** if the checkpoint actor was destroyed by the activation's own save.

## 0.2.9 - 2026-08-20

### Added

- **Instanced Meshes and Foliage:** **Replace Instance** replaces one managed ISM, HISM, or painted Static Mesh Foliage instance using compact replacement settings for the mesh, optional local transform offset, and scale preservation. The manager routes the replacement internally, preserves custom data and per-instance gameplay values, and recreates generated replacement sources for later streaming and load operations. Authored Foliage Types are never modified: a replaced foliage instance moves to a target the manager owns on the Instanced Foliage Actor.
- **World Partition Runtime Actors:** **Get World Partition Runtime Actors** returns the currently live managed actors, not just their count.
- **World Partition Runtime Actors:** **Is Managed Actor Removal In Progress** lets a managed actor tell, from **End Play**, that EMSAddons is removing it rather than gameplay destroying it. Documented alongside a pattern for NPCs that own an AI Controller.
- **Physics Streaming Guard:** new **EMS Physics Streaming Guard** component. Added to any actor with simulating physics, it holds that actor in place while the geometry underneath it is streamed out, and releases it as soon as the world below is loaded again. It reacts to streaming events rather than polling, and never freezes an actor that is simply airborne over loaded geometry.
- **Autosave and Checkpoints:** **Autosave When Leaving Map** optionally saves the current EMS Player/Level data synchronously immediately before normal map travel. It reuses EMS's native save preparation and Multi-Level merge path, ignores the minimum autosave interval, and skips safely during checkpoint travel, active EMS tasks, blockers, or unsafe streaming.

### Improved

- **Instanced Meshes and Foliage:** streamlined the Blueprint gameplay-state API to tagged value operations. The complete per-instance gameplay-data container remains internal to C++ and persistence.
- **World Partition Runtime Actors:** managed Pawns now clean up their orphaned non-player Controller automatically when EMSAddons removes them. Restored Pawns continue to use Unreal's native **Auto Possess AI** behavior; no controller persistence or extra NPC system is added.
- **Instanced Meshes and Foliage:** merged the separate **EMS Foliage Manager** into **EMS Instance Manager**. One manager now handles ISM, HISM, and painted Static Mesh Foliage, reducing setup to a single persistent manager. Existing development levels using an **EMS Foliage Manager** must replace it with an **EMS Instance Manager**.

### Fixed

- **Geometry Collection Persistence:** **EMS Geometry Collection Actor** no longer falls out of a World Partition world when the floor beneath it streams out. It now carries an **EMS Physics Streaming Guard**, so it holds position while nothing is loaded below it instead of being saved somewhere under the world.
- **Actor Spawner:** made restore safe when actor load callbacks spawn through the same spawner and modify its manifest.
- **World Partition Runtime Actors:** pre-save capture now runs on the game thread when EMS Multi-Thread Saving is enabled.
- **World Partition Runtime Actors:** **Spawn World Partition Runtime Actor** no longer rejects every spawn in a partitioned world. The cell query used a zero radius, which matches no cells at all, so every location appeared to have no visible cell.
- **World Partition Runtime Actors:** managed actors no longer disappear while the world around them stays loaded. Cell ownership is now treated as coverage: an actor stays live for as long as any visible supported cell covers its location, instead of binding it to one chosen cell whose visibility could change independently.
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
