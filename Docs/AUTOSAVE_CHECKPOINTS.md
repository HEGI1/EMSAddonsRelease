# Autosave and Checkpoints

Add autosaves, player-named saves, save menus, and checkpoints to an EMS project. Every save is an ordinary EMS slot, and the newest successful save becomes the current slot, so a **Continue** button can use a normal EMS load.

## Quick start

Get **EMS Autosave Subsystem** with **Get Game Instance Subsystem**, then:

| To… | Do this |
| --- | --- |
| Autosave at a milestone | Call **Request Autosave**. |
| Let the player name a save | Call **Manual Save** with a **Save Name**. |
| Add a checkpoint | Place an **EMS Checkpoint** and resize its **Trigger**. It activates when the player walks in. |
| Return to the last checkpoint | Call **Load Last Checkpoint**. |

The defaults work out of the box. Timed and map-travel autosaves are off until you turn them on in [Settings](#settings).

Two rules prevent the most common problems:

- **Add menu and loading maps to Excluded Maps.** Otherwise an autosave there can write the menu into the player's save. See [Menu and loading maps](#menu-and-loading-maps).
- **Don't call Set Current Save Slot on Begin Play.** Call it only when the player starts or picks a game. On Begin Play it replaces the slot that **Load Save** just selected, and the load fails.

**On this page:** [Checkpoints](#checkpoints) · [Autosaves](#autosaves) · [Manual saves](#manual-saves) · [Save menus](#save-and-load-menus) · [Menu maps](#menu-and-loading-maps) · [Multiple games](#several-games-per-player) · [Good to know](#good-to-know) · [Reference](#blueprint-nodes)

## Checkpoints

1. Place an **EMS Checkpoint** in a gameplay map.
2. Resize its **Trigger**. The trigger's center is where the player respawns.
3. The checkpoint activates when the player enters it, or when you call **Activate Checkpoint**.
4. Call **Load Last Checkpoint** to return. It opens the checkpoint's map if needed and places the player at the trigger center, keeping the saved rotation.

Use **Has Checkpoint** to decide whether to show a "Load checkpoint" option.

**Activated is not saved yet.** **On Checkpoint Activated** means the request was accepted. Wait for **On Checkpoint Committed** to know the checkpoint was written.

**Trigger Once** is on by default and is remembered across save and load. Call **Reset Checkpoint** to let it fire again.

**Checkpoint slots.** Each checkpoint save goes into its own slot, and later autosaves and manual saves keep a reference to it. **Checkpoint Slot Name** chooses that slot:

- Empty (default): all checkpoints share one slot, and each new checkpoint replaces the last.
- Different names: separate snapshots are kept. Checkpoints with the same name replace each other.

When a checkpoint slot is overwritten, older saves that pointed to the previous snapshot stop offering **Load Last Checkpoint** rather than loading the wrong one.

Checkpoint IDs are assigned automatically. Use **Regenerate Checkpoint ID** only to fix a duplicate; changing an ID breaks references in existing saves.

## Autosaves

Call **Request Autosave** at meaningful moments, such as finishing an objective. You can also turn on automatic triggers in [Settings](#settings): **Periodic Autosave Interval**, **Autosave After Map Load**, and **Autosave When Leaving Map**.

Requests wait for blockers, the **Minimum Time Between Saves**, and streaming to finish. Waiting requests can be combined into one save, so **true** means "accepted", not "written". Use the [events](#events) for the outcome.

### Keep several autosaves

**Autosave Slot Count** controls how many autosaves are kept:

| Count | Behavior |
| --- | --- |
| `0` (default) | Overwrite the profile's main slot. |
| `1` | Overwrite one dedicated autosave slot. |
| `2`–`10` | Rotate through that many slots, replacing the oldest. |

Use `2` or more if you want a fallback when a save fails or the game crashes mid-save.

### Pause autosaving

Call **Add Autosave Blocker** with a name such as `Combat` or `Cutscene`, and **Remove Autosave Blocker** with the same name afterward. Saving resumes when the last blocker is removed. Blockers are cleared on map change, so add them again on the new map if needed.

## Manual saves

Call **Manual Save** with a **Save Name**. Saving again with the same name replaces that save. An optional **Display Name** sets the menu label.

Manual saves ignore **Enable Autosave**, blockers, and the minimum save interval. They return `false` on an excluded map, while another EMS save or load is running, while streaming isn't ready, or if the name has no characters a file name can use (for example `??`).

## Save and load menus

1. Call **Get Save Slots With Details** to list the current profile's saves, newest first.
2. Show each row's **Display Name**, **Save Type** (`Autosave`, `Manual`, or `Checkpoint`), and **Time Stamp**. Keep its **Slot Name** for loading.
3. Pass the **Slot Name** to **Load Save**.
4. Finish the menu action on **On Save Loaded** or **On Save Load Failed**.

**Load Save** always reopens the saved map from a fresh world, even if the player is already there. Unsaved progress is lost. Listing saves never changes the current slot, and saves known to be incomplete are hidden.

For thumbnails, level names, or player data, use EMS's **Get Named Slot Info** and **Import Save Thumbnail** with the **Slot Name**.

### Display names

**Display Name** is the menu label; **Slot Name** is the file identity. Labels keep capitalization and spaces, support localization, and may repeat.

| Save type | Set the label with |
| --- | --- |
| Checkpoint | The checkpoint actor's **Display Name** |
| Autosave | **Set Autosave Display Name** (for example the current area), or **Request Autosave → Display Name** for one save |
| Manual | **Manual Save → Display Name**; empty uses the **Save Name** |

The autosave display name survives map travel but is not saved to disk, so set it again after starting or loading a game. **Set Save Slot Display Name** renames an existing save without touching its data. Saves without a label show their save name, `Checkpoint`, or `Autosave 1`, `Autosave 2`, and so on.

## Menu and loading maps

EMS keeps the last played slot current when the player returns to a menu. An autosave there would write the menu map into their save, and **Continue** would then open the menu.

Add every non-gameplay map to **Excluded Maps**. On those maps, the addon never saves: **Request Autosave**, **Manual Save**, and checkpoint activation return `false`, and automatic autosaves don't run. **Load Save** and **Load Last Checkpoint** still work. Your own direct EMS save calls are not affected.

## Several games per player

A **profile** groups the saves of one playthrough. Without setup, everything uses the EMS default slot as one profile.

For separate games, call **Set Current Save Slot** with a profile name such as `Game1` when the player **starts or selects** a game. Save menus then list only that profile's saves. **Get Save Profile** returns the current profile.

## Good to know

You can skip this section until you need it.

**Saving and loading through EMS**

- After a checkpoint, the current slot is the checkpoint slot. A direct EMS **Save Game Actors** call would overwrite the checkpoint, so use **Request Autosave** or **Manual Save** for later progress.
- Starting a load cancels waiting autosaves, so they can't overwrite the state being loaded.
- **Carried Custom Files** copies listed EMS custom save files into each new addon save. Fetch them with **Get Custom Save** when needed.
- Saving and loading run on the server or in standalone. Replication is your project's responsibility.
- Saves and checkpoints from version 1.1 load without migration.

**World Partition and level streaming**

- Cells or sublevels streaming in delay waiting saves; they are not canceled.
- Use EMS **1.86** or newer. With EMS 1.85, each addon save clears EMS's in-memory streaming data, and the addon logs a warning.
- If **Save Data Flags** leaves out **Level**, the slot has no Level data until a later save includes it.

**Pause menus**

**Manual Save**, **Load Save**, and **Load Last Checkpoint** to another map work while paused. **Load Last Checkpoint** on the current map returns `false`; unpause first.

If the pause interrupted an EMS streaming load, the game briefly unpauses (up to 2 seconds) to finish it, then runs the request; a timeout reports failure through the usual event. Any save or game load already running returns `false`, so disable conflicting menu buttons until the completion or failure event.

**Failed and interrupted saves**

If an addon save fails, the addon hides the incomplete slot from menus and loads and tries to switch back to the previous slot. After a crash mid-save, or if EMS blocked that switch, it tries again on the next start. This is recovery, not a backup: with **Autosave Slot Count** `0` or `1` there may be no earlier save to return to.

Changing the EMS user cancels waiting and running addon saves. Direct EMS calls bypass the addon's incomplete-save filtering.

**Save slot names**

| Save | EMS slot name |
| --- | --- |
| Autosave, count `0` | `<Profile>` |
| Autosave, count `N` | `<profile>_autosave_0` … `<profile>_autosave_<N-1>` |
| Manual save | `<profile>_manual_<name>` |
| Checkpoint | `<profile>_checkpoint`, or `<profile>_checkpoint_<slot name>` |

Generated names are lowercase, so names that differ only in case share a slot, and **Get Save Profile** may return `game1` for `Game1`. Compare profiles ignoring case, and don't end profile names with these suffixes.

## Blueprint nodes

Subsystem nodes are on **EMS Autosave Subsystem**. Checkpoint actor nodes are on **EMS Checkpoint**.

| Task | Nodes |
| --- | --- |
| Save | **Request Autosave**, **Manual Save** |
| Load and list | **Load Save**, **Get Save Slots With Details**, **Get Save Slots**, **Get Save Slot Details**, **Is Save Load In Progress** |
| Checkpoints | **Load Last Checkpoint**, **Has Checkpoint**, **Get Current Checkpoint** |
| Labels | **Set Autosave Display Name**, **Get Autosave Display Name**, **Set Save Slot Display Name** |
| Profiles | **Get Save Profile**, **Get Slot Profile** |
| Autosave control | **Add Autosave Blocker**, **Remove Autosave Blocker**, **Is Autosave Blocked**, **Is Autosave Active**, **Set Periodic Autosave Interval**, **Get Periodic Autosave Interval** |
| Custom files | **Add Carried Custom File**, **Remove Carried Custom File** |
| Checkpoint actor | **Activate Checkpoint**, **Reset Checkpoint**, **Was Activated This Session**, **Is Activation Pending** |

**Set Periodic Autosave Interval** applies to the current session only; `0` turns the timer off.

## Events

Bind these on **EMS Autosave Subsystem** before making a request. Handle a `false` return immediately; it never produces a later event.

| Event | Meaning |
| --- | --- |
| **On Autosave Started** | An addon save is starting. |
| **On Autosave Completed** / **On Autosave Failed** | The save finished or failed. Also covers manual and checkpoint saves. |
| **On Checkpoint Activated** | A checkpoint activation was accepted. Also on the checkpoint actor. |
| **On Checkpoint Committed** | The checkpoint was saved. |
| **On Checkpoint Loaded** / **On Checkpoint Load Failed** | A checkpoint load finished, failed, or was canceled. |
| **On Save Loaded** / **On Save Load Failed** | A **Load Save** finished or failed. |

Save events include a **Reason**: `Manual`, `Checkpoint`, or the Reason you passed to **Request Autosave** (default `Autosave`).

## Settings

### Project settings

**Project Settings → Easy Multi Save Addons → Autosave and Checkpoints**

| Setting | Default | Purpose |
| --- | --- | --- |
| **Enable Autosave** | On | Allows **Request Autosave** and automatic triggers. Manual saves and checkpoints work either way. |
| **Save Data Flags** | Player, Level | EMS data the addon saves and loads. |
| **Excluded Maps** | Empty | Maps where the addon never saves, such as menus and loading screens. |
| **Minimum Time Between Saves** | 5 s | Minimum gap between autosaves. `0` turns it off. Manual saves ignore it. |
| **Periodic Autosave Interval** | 0 s | Autosave on a timer. `0` turns it off. |
| **Autosave After Map Load** | Off | Autosave after a map loads, after **Map Load Autosave Delay** (1 s). |
| **Autosave When Leaving Map** | Off | Save just before normal map travel. Can cause a brief hitch. |
| **Autosave Slot Count** | 0 | See [Keep several autosaves](#keep-several-autosaves). |
| **Carried Custom Files** | Empty | EMS custom save files copied into each new addon save. |

### Checkpoint settings

Select an **EMS Checkpoint** in the level.

| Setting | Default | Purpose |
| --- | --- | --- |
| **Activate On Player Overlap** | On | Activate when a player pawn enters the trigger. |
| **Trigger Once** | On | Activate only once until **Reset Checkpoint**. |
| **Display Name** | Empty | Label shown in save menus. |
| **Checkpoint Slot Name** | Empty | Slot for this checkpoint's snapshot. Empty uses the shared default. |
