# Level Sequence

Use **EMS Level Sequence Actor** to save where a Level Sequence is in its playback, including play rate, direction, loops, and whether it has finished.

## Quick start

1. Place an **EMS Level Sequence Actor** instead of the standard Level Sequence Actor.
2. Assign its Level Sequence and set up playback as usual.
3. Save and load normally through EMS.

It works in streamed levels and World Partition.

## Good to know

- Playing, paused, stopped, and finished states are all restored, along with loop progress.
- To keep a cutscene paused after loading until gameplay starts it, turn off **Resume If Playing**.
- If the sequence's actors are in an area that hasn't streamed in yet, restoration waits up to **Restore Timeout**, then restores what it can.
- Saving and restoration run on the server or in standalone. Replication is your project's responsibility.

## Blueprint nodes

| Task | Nodes |
| --- | --- |
| Play forward with a loop count | **Play Looping** (`-1` forever, `0` once, `N` for N repeats) |
| Check whether restoration finished, and how | **Is EMS Addon Restore Complete**, **Get EMS Addon Restore Result** |

The normal Level Sequence playback nodes still work. Use the actor as **Target**. "Complete" doesn't always mean "succeeded"; check the result.

## Events

| Event | Meaning |
| --- | --- |
| **On Level Sequence Restore Started** | Restoring saved playback began. |
| **On Level Sequence Restore Finished** | Restoring ended. Check the result. |

## Settings

| Setting | Default | Purpose |
| --- | --- | --- |
| **Resume If Playing** | On | Resume a sequence that was playing when saved. Off leaves it paused. |
| **Position Restore Policy** | Jump And Evaluate | **Jump And Evaluate** goes straight to the saved time. **Traverse And Evaluate** also evaluates the frames in between. |
| **Restore Timeout** | 5 s | How long to wait for the sequence's actors (0.05–60 s). |
