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

- **Get Save Slots** lists the profile's saves, newest first. Use EMS **Get Named Slot Info** and **Import Save Thumbnail** for further details such as level and players.
- **Get Save Slots With Details** returns the same list with **Slot Name**, **Display Name**, **Save Type** (Autosave, Manual, Checkpoint), and **Time Stamp**. **Get Save Slot Details** reads one existing slot; a missing slot returns false. These nodes do not change the current slot.
- **Load Save** makes a slot current, opens its map and loads it. **On Save Loaded** / **On Save Load Failed** report the result.
- **Get Save Profile** / **Get Slot Profile** return the profile of the current slot or of any slot name.

## Display names

Display names are optional labels stored per save. The menu can show exactly `Factory Inner` while the internal EMS slot is `game1_checkpoint_factoryinner`. Use **Display Name** for menu text and **Slot Name** for **Load Save**, thumbnails, and EMS operations. Duplicate display names are allowed and never decide which save is overwritten.

- **Checkpoints:** set the checkpoint actor's **Display Name** to `Factory Inner`. **Checkpoint Slot Name** still decides which checkpoints share and replace a save.
- **Autosaves:** call **Set Autosave Display Name** when the player enters a location. Future gameplay, periodic, map-load, and map-leave autosaves capture that name. **Request Autosave** also has an optional **Display Name** input that overrides it for one request. Empty uses the current autosave display name. Queued requests keep the name captured when requested; when ordinary requests combine, the latest request's name wins.
- **Manual saves:** **Manual Save** has an optional **Display Name** input for player-entered text. Empty uses **Save Name**. **Save Name** remains the internal identifier: calling with the same Save Name replaces that manual save, even when its display name differs. Use distinct Save Names for separate saves with identical labels.
- **Renaming:** call **Set Save Slot Display Name** with an existing Slot Name. This changes only its label, preserving the save data, timestamp, and rotation order. It returns false while saving/loading, on clients, or if the slot does not exist. Empty or whitespace clears the alias and uses a fallback.

Existing saves work without migration. Named manual/checkpoint slots fall back to their name suffix; unnamed checkpoints show `Checkpoint`; autosaves show `Autosave` or `Autosave 1`, `Autosave 2`, etc. Existing checkpoint records use their stored Display Name only for the actual checkpoint slot, not for later saves carrying that record.

Names use Text, preserving spaces, punctuation, and localized checkpoint labels without filename sanitization. A name is saved with its snapshot; moving to a new location does not relabel older saves. **Set Autosave Display Name** is a game-session value: initialize/update it from your game's location state on new game, load, and map changes. It survives map travel and is not restored automatically from a loaded save.

The addon stores these labels in the slot-bound custom file `EMSSlotMetadata`. It is internal and excluded from **Carried Custom Files**, so one save cannot inherit another save's label. A later save to the same slot replaces its label with that save's name. No change to EMS core or existing slot names is required.

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

- With World Partition or level streaming, EMS keeps restoring streamed cells and sub-levels from memory only when the game switches to a slot without Level data. So a save that replaces an existing slot first deletes that slot's old Level data. Until the save finishes, that slot cannot be loaded. This applies when **Save Data Flags** do not include Level too: that save never writes Level data, so the slot has none until the game saves Level data to it again.
- With streaming, a failed save returns to the previous slot, which has Level data. EMS then stops restoring streamed cells from memory: World Partition reloads the level from the previous slot when the next cell streams in. The addon logs a warning.
- Do not call **Set Current Save Slot** on Begin Play. It replaces the slot **Load Save** selected, and the load fails.
- Your own **Save Game Actors** call writes the current slot. Right after a checkpoint, that is the checkpoint slot, so the checkpoint ends. Use **Request Autosave** or **Manual Save** instead.
- While paused, **Load Last Checkpoint** for a checkpoint on the current map returns false, because EMS does not load during pause. Unpause first.
- When a save's map no longer exists, **Load Save** and **Load Last Checkpoint** report failure and the previous slot becomes current again.
- When EMS reports that a save to another slot failed, the addon returns to the previous slot and deletes the incomplete one. A save that rewrites the current slot (**Autosave Slot Count** 0 or 1) has no slot to return to.
- Some unfinished saves return to the previous slot but keep their target: a save canceled before EMS starts writing (the target keeps its older actor data, but already has the newer **Carried Custom Files**), a save cut off by map travel, and a checkpoint whose record could not be written. With streaming, a canceled save already deleted the target's old Level data, so the addon deletes that target.
- EMS refuses the switch back while another EMS operation runs, for example one started from **On Autosave Started**. The game then stays on the unfinished target, and the next start returns to the previous slot as after a crash.
- After a crash during a save, the next start returns to the previous slot when it can, deletes the incomplete slot and logs a warning. The same applies when travel cut a save off and play went on in that slot, unless a later complete save rewrote it.
- **Autosave When Leaving Map** saves synchronously and can cause a brief hitch. It is skipped while EMS is busy or blocked, and for **Load Save** travel.
- Starting a load cancels pending autosaves.
- Saves and checkpoints from 1.1 keep working.
- Slot-bound EMS custom saves listed in **Carried Custom Files** are copied into each new save. Fetch them with **Get Custom Save** when needed instead of keeping the object across saves.
- Checkpoint IDs are assigned automatically. Use **Regenerate Checkpoint ID** only to fix a duplicate; changing an established ID breaks existing saves.

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
| **Autosave Slot Count** | 0 | `0` saves to the profile slot (as in 1.1); `1`–`10` rotate autosave slots. |
| **Carried Custom Files** | Empty | Slot-bound custom save files copied into each new save. |

**EMS Checkpoint**

| Setting | Default | Meaning |
| --- | --- | --- |
| **Activate On Player Overlap** | On | Activate when a player enters the trigger. |
| **Trigger Once** | On | Activate only once until **Reset Checkpoint**. |
| **Display Name** | Empty | Optional save-menu label, independent of the EMS slot name. |
| **Checkpoint Slot Name** | Empty | Optional name of the checkpoint's slot, stored lowercase. |
