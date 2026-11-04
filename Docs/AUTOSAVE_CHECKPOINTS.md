# Autosave and Checkpoints

Use **Autosave and Checkpoints** to request automatic EMS saves and provide gameplay checkpoints. It uses your normal EMS save slots.

## Basic use

1. Open **Project Settings → Easy Multi Save Addons → Autosave and Checkpoints**.
2. Choose the EMS data to save, usually **Player** and **Level**.
3. Enable the autosave triggers you need, or call **Request Autosave** from gameplay.
4. To use checkpoints, place an **EMS Checkpoint** and configure its trigger.

**Enable Autosave** controls ordinary autosave requests. Checkpoint activation is independent and always starts a checkpoint save.

## Checkpoints

1. Place an **EMS Checkpoint** actor and size its Trigger volume.
2. Enter the trigger or call **Activate Checkpoint**.
3. Call **Load Last Checkpoint** when gameplay should return to it.

After loading, the EMS player is positioned at the checkpoint trigger's center. Checkpoints can restore across maps and streamed areas.

Checkpoint options:

- **Activate On Player Overlap** — activate when a player enters the trigger.
- **Trigger Once** — prevent repeat activation until reset.
- **Display Name** — optional checkpoint name.

**Reset Checkpoint** resets the actor's activation state. IDs are assigned automatically; use **Regenerate Checkpoint ID** only to resolve a duplicate. Changing an established ID breaks the match with previously saved checkpoint destinations.

## Autosaves

Call **Request Autosave** for gameplay-driven saves. Optional triggers include:

- **Periodic Autosave Interval** — set to `0` to disable.
- **Autosave After Map Load** — save after entering a map.
- **Autosave When Leaving Map** — save before map travel (disabled by default).

Map-leave autosaves are synchronous and may cause a brief hitch. They skip the normal minimum interval but cannot run while another save/load, a blocker, or an unsafe streaming state prevents saving.

Use **Add Autosave Blocker** and **Remove Autosave Blocker** to pause autosaves temporarily, such as during combat or cutscenes. Other autosave requests can be delayed or combined until saving is possible.

## Blueprint nodes and events

Use **Get Game Instance Subsystem** to access **EMS Autosave Subsystem** for autosave requests, checkpoint loading, status queries, and events. **Activate Checkpoint**, **Reset Checkpoint**, and the session-activation queries belong to the **EMS Checkpoint** actor.

Common nodes:

- **Request Autosave**
- **Activate Checkpoint** / **Load Last Checkpoint**
- **Has Checkpoint** / **Get Current Checkpoint**
- **Reset Checkpoint**
- **Add Autosave Blocker** / **Remove Autosave Blocker**
- **Is Autosave Active** / **Is Autosave Blocked**
- **Set Periodic Autosave Interval** / **Get Periodic Autosave Interval**
- **Was Activated This Session** / **Is Activation Pending** (checkpoint actor)

Events include **On Autosave Started**, **On Autosave Completed**, **On Autosave Failed**, **On Checkpoint Activated**, **On Checkpoint Committed**, **On Checkpoint Loaded**, and **On Checkpoint Load Failed**. The checkpoint actor also exposes **On Checkpoint Activated**.

## Important behavior

- Checkpoints belong to the current EMS save slot; they are not separate historical save files.
- A later successful normal EMS save supersedes the loadable checkpoint.
- Starting a load cancels pending addon autosaves so they cannot overwrite the restored game.
- Startup overlaps do not immediately reactivate a checkpoint after loading.
- Normal EMS save and load behavior remains available.
