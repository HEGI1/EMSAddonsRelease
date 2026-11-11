# Autosave and Checkpoints

Use **Autosave and Checkpoints** for automatic saves, manual saves, save slots and gameplay checkpoints. Every save is an ordinary EMS slot, so **Continue** is a normal EMS load.

## Basic use

1. Open **Project Settings → Easy Multi Save Addons → Autosave and Checkpoints** and choose the EMS data to save, usually **Player** and **Level**.
2. Add your main menu, front-end and loading maps to **Excluded Maps**. See [Menu and loading maps](#menu-and-loading-maps).
3. Set the profile with **Set Current Save Slot** (for example `Game1`) when the player starts or picks a game.
4. Call **Request Autosave** from gameplay, or enable the automatic triggers.
5. Call **Manual Save** for player-named saves.
6. Place an **EMS Checkpoint** and call **Load Last Checkpoint** to return to it.

Use **Get Game Instance Subsystem** to access **EMS Autosave Subsystem**.

## Save slots

All saves of one game share a **profile**, the current EMS slot name without the addon's suffix. Each save becomes the current EMS slot.

| Save | EMS slot |
| --- | --- |
| Autosave, **Autosave Slot Count** 0 | `<Profile>` |
| Autosave, **Autosave Slot Count** N | `<profile>_autosave_0` … `<profile>_autosave_<N-1>`; unused slots first, then the oldest |
| **Manual Save** | `<profile>_manual_<name>` |
| Checkpoint | `<profile>_checkpoint`, or `<profile>_checkpoint_<checkpoint slot name>` |

Generated slot IDs are lowercase, so manual save names and **Checkpoint Slot Name** values that differ only in case share a slot. With **Autosave Slot Count** 0 the profile slot keeps its spelling, and existing mixed-case saves keep their paths and are still reused. After a save to a generated slot, **Get Save Profile** returns the lowercase profile (for example `game1`), so compare profiles ignoring case.

EMS removes characters a file name cannot hold, such as `?`, `*` or `:`. **Manual Save** returns false for a **Save Name** with nothing left, such as `??`; a **Checkpoint Slot Name** like that uses the default checkpoint slot.

Do not end profile names with the reserved suffixes `_autosave_<0-9>`, `_manual_<name>`, `_checkpoint` or `_checkpoint_<name>`, in any case.

## Menu and loading maps

EMS keeps the last played slot current, also in your main menu. A save there would write the menu map into the player's profile; with **Autosave Slot Count** 0 it replaces their progress, and **Load Save** or Continue would open the menu.

List every map that is not gameplay in **Excluded Maps**. There the addon never saves: **Request Autosave**, **Manual Save** and **Activate Checkpoint** return false, checkpoint triggers do not save, and no automatic trigger fires, including **Autosave When Leaving Map** on the way out. **Load Save** and **Load Last Checkpoint** still work.

## Save and load menus

- **Get Save Slots** lists the profile's saves, newest first. Use EMS **Get Named Slot Info** and **Import Save Thumbnail** for details such as level and players.
- **Get Save Slots With Details** adds **Slot Name**, **Display Name**, **Save Type** (Autosave, Manual, Checkpoint) and **Time Stamp**. **Get Save Slot Details** reads one slot and returns false if it is missing. Neither node changes the current slot.
- **Load Save** makes a slot current, opens its map and loads it. **On Save Loaded** / **On Save Load Failed** report the result.
- **Get Save Profile** / **Get Slot Profile** return the profile of the current slot or of any slot name.

These nodes skip saves known to be incomplete; see [Failure handling and recovery](#failure-handling-and-recovery).

## Display names

Use **Display Name** for menu text, such as `Factory Inner`, and **Slot Name** for loading and EMS operations. Labels are Text, so they keep capitalization, spaces, punctuation and localization. Duplicate labels are allowed.

| Save | Set its label |
| --- | --- |
| Checkpoint | The actor's **Display Name**. **Checkpoint Slot Name** selects the slot. |
| Autosave | **Set Autosave Display Name** when the location changes. **Request Autosave → Display Name** overrides it for one request. |
| Manual | **Manual Save → Display Name**; empty uses **Save Name**, which selects the slot. |

Names are captured when requested; combined requests keep the latest. The current autosave name survives map travel but is not saved, so set it again on new game, load and map change.

**Set Save Slot Display Name** renames a slot without changing its data, timestamp or rotation order. Empty or whitespace clears the label. It returns false while busy, on clients, or for a missing slot.

Saves without a label show a fallback: the manual or checkpoint name, `Checkpoint`, or `Autosave` / `Autosave 1` and so on.

## Checkpoints

1. Place an **EMS Checkpoint** and size its Trigger volume.
2. The player enters the trigger, or call **Activate Checkpoint** on the actor.
3. Call **Load Last Checkpoint** to return. The player is placed at the trigger's center, also across maps.

Checkpoints have their own slots, so autosaves and manual saves do not replace them, and each save remembers its checkpoint. Checkpoints with the same **Checkpoint Slot Name** share one slot. Use **Reset Checkpoint** to let a **Trigger Once** checkpoint activate again.

Reactivating a checkpoint replaces its snapshot. Older saves that referred to the replaced snapshot no longer offer **Load Last Checkpoint**.

## Autosave blockers

Use **Add Autosave Blocker** / **Remove Autosave Blocker** to hold autosaves during combat or cutscenes. Held requests are delayed or combined until saving is allowed. **Manual Save** ignores blockers, **Enable Autosave** and the minimum interval.

Leaving the map clears all blockers, including ones added from the Game Instance. Add them again on the new map if needed.

## Blueprint nodes

- **Request Autosave** / **Manual Save**
- **Load Save** / **Get Save Slots** / **Get Save Profile** / **Get Slot Profile** / **Is Save Load In Progress**
- **Get Save Slots With Details** / **Get Save Slot Details** / **Set Save Slot Display Name**
- **Set Autosave Display Name** / **Get Autosave Display Name**
- **Load Last Checkpoint** / **Has Checkpoint** / **Get Current Checkpoint**
- **Add Carried Custom File** / **Remove Carried Custom File**
- **Add Autosave Blocker** / **Remove Autosave Blocker** / **Is Autosave Blocked** / **Is Autosave Active**
- **Set Periodic Autosave Interval** / **Get Periodic Autosave Interval**
- Checkpoint actor: **Activate Checkpoint** / **Reset Checkpoint** / **Was Activated This Session** / **Is Activation Pending**

## Events

- **On Autosave Started** / **On Autosave Completed** / **On Autosave Failed** (Reason `Manual` for manual saves)
- **On Checkpoint Activated** (also on the checkpoint actor) / **On Checkpoint Committed** / **On Checkpoint Loaded** / **On Checkpoint Load Failed**
- **On Save Loaded** / **On Save Load Failed**

## Important behavior

- Do not call **Set Current Save Slot** on Begin Play. It replaces the slot **Load Save** selected, and the load fails.
- Your own **Save Game Actors** call writes the current slot. Right after a checkpoint, that is the checkpoint slot, so the checkpoint ends. Use **Request Autosave** or **Manual Save** instead.
- **Autosave When Leaving Map** saves synchronously and can cause a brief hitch. It is skipped while EMS is busy or autosaves are blocked, and for **Load Save** travel.
- Starting a load cancels pending autosaves. When a save's map no longer exists, **Load Save** and **Load Last Checkpoint** report failure and the previous slot becomes current again.
- **Carried Custom Files** (slot-bound EMS custom saves) are copied into each new save. Fetch them with **Get Custom Save** when needed instead of keeping the object across saves.
- Checkpoint IDs are assigned automatically. Use **Regenerate Checkpoint ID** only to fix a duplicate; changing an established ID breaks existing saves.
- Saves and checkpoints from 1.1 keep working without migration.

### Pause menus

- While paused, **Load Last Checkpoint** for a checkpoint on the current map returns false, because EMS does not load during pause.
- A pause can stop an EMS streaming load mid-task, such as a World Partition cell loading. **Manual Save**, **Load Save** and a map-changing **Load Last Checkpoint** still work then: the game runs until that work finishes (at most 2 seconds), pauses again and the request runs. Meanwhile **Is Autosave Active** or **Is Save Load In Progress** returns true, so the menu can keep Resume disabled.
- If that wait times out, or the EMS user or profile changes during it, the request reports failure once through its usual event. A running game load or any active save, including an ordinary Level-only disk save, makes the request return false and leaves the pause unchanged. EMS 1.85/1.86 do not publicly expose the flags identifying memory-only streaming saves, so those saves also require unpausing first; only identifiable streaming loads are waited out.

### Streaming and EMS versions

- A World Partition cell or sub-level streaming in only delays a save. **Request Autosave** and checkpoint activation are accepted during it and save once it has finished. A game load (**Load Game Actors**) refuses them and cancels pending requests.
- With EMS 1.86, an addon save removes its target's old Level file before switching slots, so World Partition and level streaming keep restoring from memory. If the removal fails, the save is refused. The target cannot be loaded until the save finishes. A save without Level in **Save Data Flags** leaves the slot without Level data until a later Level save.
- EMS 1.85 clears streaming memory when switching slots. The addon logs a warning.
- When a save fails under streaming, the previous slot becomes current again and World Partition reloads streamed cells from it. The addon logs a warning.

## Failure handling and recovery

- Requests and running saves belong to the EMS user and profile that made them. Changing either, also from **On Autosave Started**, cancels them. Cleanup never switches to or deletes another user's slots.
- A save succeeds only when EMS has written fresh Slot Info, the display metadata and, for checkpoints, the checkpoint record. On failure, the previous slot becomes current again and the incomplete target is deleted where possible. A save into the current slot has nothing to return to; **Autosave Slot Count** 0 or 1 can do that, 2 or more never replaces the current autosave.
- Before changing checkpoint records or **Carried Custom Files**, the addon persists an exclusion for the target and its EMS user in the `EmsUser` config. Preparation has no full rollback: a canceled save, failed copy or travel interruption can leave older actors beside newer custom data, but that target stays hidden from addon menus, details, checkpoint resolution and loads, including after restart. This also applies to **Autosave When Leaving Map** and saves into the current slot.
- The exclusion takes effect when a save fails, is canceled or is cut off. While the save is still running, its target stays listed and **Has Checkpoint** / **Get Current Checkpoint** keep reporting the carried checkpoint, so a menu opened during an autosave still shows the slot being written (with **Autosave Slot Count** 0, the profile's only slot).
- A save into the current slot that is cut off by map travel or quitting leaves that slot excluded until the next completed addon save into it, also after a restart. With **Autosave Slot Count** 0 or 1 that can be the profile's newest autosave; use 2 or more if an earlier autosave must always remain selectable.
- A successful, fully committed addon replacement clears the exclusion; successful cleanup deletion clears it too. Cancellation or returning to the previous slot does not clear it. Direct EMS calls are not restricted and do not clear the exclusion.
- After a crash, or when EMS refused to switch back, the next start returns to the previous save if the current user and slot still match, then deletes the incomplete target. A failed switch is retried on the following start; a failed deletion is reported and the target stays excluded across restarts. The exclusion is independent of the recovery journal, so a partial copy is protected even when the target never became current.

## Settings

**Project Settings → Easy Multi Save Addons → Autosave and Checkpoints**

| Setting | Default | Meaning |
| --- | --- | --- |
| **Enable Autosave** | On | Allows **Request Autosave** and the automatic triggers. Checkpoints work either way. |
| **Save Data Flags** | Player, Level | EMS data the addon saves and loads. |
| **Excluded Maps** | Empty | Maps where the addon never saves, such as the main menu and loading maps. |
| **Minimum Time Between Saves** | 5 s | Earlier requests wait. `0` disables it. |
| **Periodic Autosave Interval** | 0 s | Autosave at this interval. `0` disables it. |
| **Autosave After Map Load** / **Map Load Autosave Delay** | Off / 1 s | Autosave this long after a map opens. |
| **Autosave When Leaving Map** | Off | Save just before map travel. |
| **Autosave Slot Count** | 0 | `0` saves to the profile slot (as in 1.1); `1` reuses one autosave slot; `2`–`10` rotate slots. |
| **Carried Custom Files** | Empty | Slot-bound custom save files copied into each new save. |

**EMS Checkpoint**

| Setting | Default | Meaning |
| --- | --- | --- |
| **Activate On Player Overlap** | On | Activate when a player enters the trigger. |
| **Trigger Once** | On | Activate only once until **Reset Checkpoint**. |
| **Display Name** | Empty | Optional save-menu label, independent of the EMS slot name. |
| **Checkpoint Slot Name** | Empty | Optional name of the checkpoint's slot, stored lowercase. |
