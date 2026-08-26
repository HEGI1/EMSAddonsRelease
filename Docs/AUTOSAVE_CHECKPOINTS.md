# Autosave and Checkpoints

`EMSAddonsAutosave` adds ready-to-use autosaves and gameplay checkpoints on top of Easy Multi Save. It uses the normal EMS save/load system and only decides when to save and how checkpoint restore should behave.

## Setup

Open **Project Settings → Easy Multi Save Addons → Autosave and Checkpoints** and choose which EMS data the addon should save, usually **Player** and **Level**.

**Enable Autosave** controls general autosave requests and automatic triggers. Checkpoints are independent: activating a checkpoint always saves because that save is the checkpoint.

## Checkpoints

1. Place an **EMS Checkpoint** actor and size its Trigger volume.
2. Enter the trigger or call **Activate Checkpoint**.
3. The addon saves the configured EMS data and stores that checkpoint for the current EMS save slot.
4. Call **Load Last Checkpoint** to restore it.

After EMS restores the saved state, the player is placed at the **center of the checkpoint Trigger**. Restored rotation and other player state are retained.

Checkpoint loading can travel to another map. In streamed levels and World Partition, placement waits for the matching checkpoint to stream in when necessary.

### Checkpoint options

- **Activate On Player Overlap** — activates when the player enters the Trigger.
- **Trigger Once** — allows one activation per play session. Disable for repeatable checkpoints. **Activate Checkpoint** returns false when the committed checkpoint already matches, so a checkpoint rebuilt by a respawn or a streamed-level reload does not report a second activation.
- **Display Name** — optional saved checkpoint name.

Use **Reset Checkpoint** to clear the checkpoint actor's session activation state.

Startup and load-generated overlaps are ignored, so restoring inside a checkpoint does not immediately save again.

## Autosaves

Call **Request Autosave** whenever gameplay should request a normal EMS save.

Requests are combined while EMS is busy, streaming is not ready, a blocker is active, or the minimum save interval has not passed.

Optional automatic triggers:

- **Periodic Autosave Interval** — repeated autosaves; `0` disables them.
- **Autosave After Map Load** — requests an autosave after loading a map.
- **Autosave When Leaving Map** — saves immediately before normal map travel begins.

### Autosave when leaving a map

Off by default. When enabled, the outgoing map is saved at Unreal's pre-load-map boundary, while its world and actors are still valid.

This save runs **synchronously**, so expect a brief hitch during map travel. A queued asynchronous save is not safe here because `LoadMap` continues into world teardown before that task can be guaranteed to finish.

Two differences from the other triggers are deliberate:

- **The minimum save interval does not apply.** Leaving a map happens once, so throttling could discard the save the option exists to guarantee.
- **It is not a request.** The other triggers queue through the pending-request path and may be combined or delayed; this one either saves immediately or is skipped because there is no later point in the outgoing world to retry.

It is skipped, with a log line naming the reason, when an autosave blocker is active, another EMS save/load is running, no save slot is set, level streaming is not ready, or a checkpoint load is travelling to its restore map. That checkpoint case prevents the outgoing world from overwriting the slot that is about to be restored.

The save uses the same configured **Player** and **Level** data flags as the other addon autosaves. EMS performs its normal save preparation, serialization, and Multi-Level merge; EMSAddons does not maintain a separate level archive or merge path.

A successful save supersedes a loadable checkpoint exactly like any other normal **Save Game Actors** operation.

Runtime interval nodes:

- **Set Periodic Autosave Interval**
- **Get Periodic Autosave Interval**

## Autosave blockers

Use named blockers when autosaving should temporarily wait, for example during combat or cinematics.

- **Add Autosave Blocker**
- **Remove Autosave Blocker**

Saving can continue after the final blocker is removed.

## Blueprint nodes

- **Request Autosave**
- **Activate Checkpoint**
- **Load Last Checkpoint**
- **Has Checkpoint**
- **Get Current Checkpoint**
- **Reset Checkpoint**
- **Add Autosave Blocker**
- **Remove Autosave Blocker**
- **Is Autosave Active**
- **Is Autosave Blocked**
- **Set Periodic Autosave Interval**
- **Get Periodic Autosave Interval**

## Events

The subsystem exposes **On Autosave Started**, **On Autosave Completed**, **On Autosave Failed**, **On Checkpoint Activated**, **On Checkpoint Committed**, **On Checkpoint Loaded**, and **On Checkpoint Load Failed**.

The checkpoint actor also exposes **On Checkpoint Activated**.

## Important behavior

- A checkpoint belongs to the current EMS save slot.
- A later successful normal **Save Game Actors** call supersedes that checkpoint state. EMSAddons does not create a hidden historical snapshot slot.
- Loading cancels pending addon autosaves so restored state is not immediately overwritten.
- Normal EMS slots, users, manual saves, loading, streaming, and serialization remain unchanged.
