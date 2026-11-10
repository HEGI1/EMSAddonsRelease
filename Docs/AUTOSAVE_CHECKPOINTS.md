# Autosave and Checkpoints

Use **Autosave and Checkpoints** for automatic saves, manual saves, save slots and gameplay checkpoints. Every save is an ordinary EMS slot, so **Continue** is a normal EMS load.

## Basic use

1. Open **Project Settings → Easy Multi Save Addons → Autosave and Checkpoints** and choose the EMS data to save, usually **Player** and **Level**.
2. Set the profile with **Set Current Save Slot** (for example `Game1`) when the player starts or picks a game.
3. Call **Request Autosave** from gameplay, or enable the automatic triggers.
4. Call **Manual Save** for player-named saves.
5. Place an **EMS Checkpoint** and call **Load Last Checkpoint** to return to it.

Use **Get Game Instance Subsystem** to access **EMS Autosave Subsystem**.

## Save slots

All saves of one game share a **profile**, the current EMS slot name without the addon's suffix. Each save becomes the current EMS slot.

| Save | EMS slot |
| --- | --- |
| Autosave, **Autosave Slot Count** 0 | `<Profile>` |
| Autosave, **Autosave Slot Count** N | `<profile>_autosave_0` … `<profile>_autosave_<N-1>`; unused slots first, then the oldest |
| **Manual Save** | `<profile>_manual_<name>` |
| Checkpoint | `<profile>_checkpoint`, or `<profile>_checkpoint_<checkpoint slot name>` |

Generated slot IDs are lowercase, including the profile and name parts. Existing mixed-case saves retain their paths and are reused when saving to the same ID. With **Autosave Slot Count** 0, the caller-supplied profile slot remains unchanged. Display names keep their original capitalization.

Because IDs are lowercase, manual save names and **Checkpoint Slot Name** values that differ only in case use the same slot. After a save to a generated slot, **Get Save Profile** returns the lowercase profile (for example `game1`), so compare profiles ignoring case.

These endings are reserved regardless of case, so do not use them in profile names: `_autosave_<0-9>`, `_manual_<name>`, `_checkpoint`, `_checkpoint_<name>`.

## Save and load menus

- **Get Save Slots** lists the profile's saves, newest first, excluding targets known to be incomplete in this game instance. Use EMS **Get Named Slot Info** and **Import Save Thumbnail** for further details such as level and players.
- **Get Save Slots With Details** adds **Slot Name**, **Display Name**, **Save Type** (Autosave, Manual, Checkpoint), and **Time Stamp**. **Get Save Slot Details** reads one slot and returns false if it is missing or excluded as incomplete. Neither node changes the current slot.
- **Load Save** makes a slot current, opens its map and loads it. **On Save Loaded** / **On Save Load Failed** report the result.
- **Get Save Profile** / **Get Slot Profile** return the profile of the current slot or of any slot name.

## Display names

Use **Display Name** for menu text, such as `Factory Inner`, and **Slot Name** for loading and EMS operations. Labels use Text and preserve capitalization, spaces, punctuation and localization. Duplicate labels are allowed; slot IDs decide which save is replaced.

| Save | Set its label |
| --- | --- |
| Checkpoint | Set the actor's **Display Name**. **Checkpoint Slot Name** selects the slot. |
| Autosave | Call **Set Autosave Display Name** when the location changes. **Request Autosave → Display Name** overrides it for one request; empty uses the current name. |
| Manual | Pass **Display Name** to **Manual Save**; empty uses **Save Name**. **Save Name** selects the slot. |

Names are captured when requested, including while queued. Combined ordinary requests keep the latest name; changing location does not relabel older saves. The current autosave name survives map travel but is not restored by loading: update it from your game's location state on new game, load and map changes.

**Set Save Slot Display Name** renames an existing slot without changing its data, timestamp or rotation order. Empty or whitespace clears the label. It returns false while busy, on clients, or for a missing or excluded slot.

Older saves need no migration. Fallback labels use the manual/checkpoint suffix, `Checkpoint`, or `Autosave` / `Autosave 1`, etc. An older checkpoint's stored label applies only to its own slot.

## Checkpoints

1. Place an **EMS Checkpoint** and size its Trigger volume.
2. The player enters the trigger, or call **Activate Checkpoint** on the actor.
3. Call **Load Last Checkpoint** to return. The player is placed at the trigger's center, also across maps.

Checkpoints have their own slots, so autosaves and manual saves do not replace them, and each save remembers its checkpoint. Checkpoints with the same **Checkpoint Slot Name** (ignoring case) share one slot. Use **Reset Checkpoint** to allow a **Trigger Once** checkpoint to activate again.

## Autosave blockers

Use **Add Autosave Blocker** / **Remove Autosave Blocker** to hold autosaves during combat or cutscenes. Held requests are delayed or combined until saving is allowed. **Manual Save** ignores blockers, **Enable Autosave** and the minimum interval.

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
- **On Checkpoint Activated** / **On Checkpoint Committed** / **On Checkpoint Loaded** / **On Checkpoint Load Failed**
- **On Save Loaded** / **On Save Load Failed**

The checkpoint actor also has **On Checkpoint Activated**.

## Important behavior

- With EMS 1.86, World Partition and level streaming keep restoring from memory only when switching to a slot without Level data. Before switching, an addon save therefore removes its target's old Level file and refuses the switch if removal fails. Until saving finishes, the modified target cannot be loaded. A save without Level in **Save Data Flags** leaves the slot without Level data until a later Level save.
- EMS 1.85 supports ordinary slot switching but clears streaming memory restoration when switching. The addon logs a warning; the EMS 1.86 continuity workaround does not apply to 1.85.
- With streaming, a failed save returns to the previous slot, which has Level data. EMS then stops restoring streamed cells from memory: World Partition reloads the level from the previous slot when the next cell streams in. The addon logs a warning.
- Do not call **Set Current Save Slot** on Begin Play. It replaces the slot **Load Save** selected, and the load fails.
- Your own **Save Game Actors** call writes the current slot. Right after a checkpoint, that is the checkpoint slot, so the checkpoint ends. Use **Request Autosave** or **Manual Save** instead.
- While paused, **Load Last Checkpoint** for a checkpoint on the current map returns false, because EMS does not load during pause. Unpause first.
- When a save's map no longer exists, **Load Save** and **Load Last Checkpoint** report failure and the previous slot becomes current again.
- **Autosave When Leaving Map** saves synchronously and can cause a brief hitch. It is skipped while EMS is busy or blocked, and for **Load Save** travel.
- Starting a load cancels pending autosaves.
- Saves and checkpoints from 1.1 keep working.
- Slot-bound EMS custom saves listed in **Carried Custom Files** are copied into each new save. Fetch them with **Get Custom Save** when needed instead of keeping the object across saves.
- Checkpoint IDs are assigned automatically. Use **Regenerate Checkpoint ID** only to fix a duplicate; changing an established ID breaks existing saves.

## Failure handling and recovery

- Pending requests belong to the EMS user and profile that requested them. Changing either cancels the request before it starts. Changing the user from **On Autosave Started** also cancels the save; cleanup never switches or deletes the new user's slots.
- Success requires fresh Slot Info, display metadata and, for checkpoints, a written checkpoint record. A failed save returns to the previous slot and attempts to delete the incomplete target. When saving into the current slot, there is no earlier slot to return to. **Autosave Slot Count** 0 or 1 can reuse the current slot; a count of at least 2 avoids replacing the current autosave.
- A save canceled before EMS writes can keep its target's older actor data with newer **Carried Custom Files**. A save interrupted by travel can also keep its target. If preparation removed the old Level data, cancellation attempts to delete that target instead.
- Carried source files are checked before target preparation. Preparation has no full rollback: later write failures can leave changed custom data. A failed carry operation or cleanup excludes the incomplete target from addon menus, details and loads for this game instance and EMS user. Successful addon replacement makes it available again. This exclusion does not persist across restarts or restrict direct EMS calls.
- EMS can refuse to return to the previous slot while another task runs. The interrupted-save journal is retained so the next start can recover. Checkpoint saves keep that journal until their record is written.
- After a crash, recovery returns to the previous existing save when the journal still matches the current user and slot, then attempts to delete the incomplete target. A failed switch retains the journal for retry. A successful switch clears it; failed deletion is reported and the target is excluded for the session. Deletion is not durably retried through another crash. A later successful EMS save covering all addon data clears a matching journal left by an interrupted save.

## Settings

**Project Settings → Easy Multi Save Addons → Autosave and Checkpoints**

| Setting | Default | Meaning |
| --- | --- | --- |
| **Enable Autosave** | On | Allows **Request Autosave** and the automatic triggers. Checkpoints work either way. |
| **Save Data Flags** | Player, Level | EMS data the addon saves and loads. |
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
