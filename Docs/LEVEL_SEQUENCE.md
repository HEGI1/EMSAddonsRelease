# Level Sequence Persistence

Use **EMS Level Sequence Actor** to save playback position, play rate, direction, loop progress, and finished state through EMS.

## Basic use

1. Place an **EMS Level Sequence Actor** instead of the standard Level Sequence Actor.
2. Assign its Level Sequence asset and configure playback.
3. Adjust the restore options if needed.
4. Save and load normally through EMS.

The actor supports streamed levels and World Partition.

## Restore options

- **Resume If Playing** — automatically resumes a sequence that was playing when saved.
- **Position Restore Policy** — **Jump And Evaluate** goes directly to the saved position; **Traverse And Evaluate** also evaluates the intervening frames.
- **Restore Timeout** — limits how long restoration waits for required playback objects and bindings.

## Blueprint nodes and events

- **Is EMS Addon Restore Complete** / **Get EMS Addon Restore Result** — check restoration status on the actor.
- **Play Looping** — play a specified number of loops: `-1` for infinite playback, `0` for a single play, or a positive number for that many repeats.

Events: **On Level Sequence Restore Started** and **On Level Sequence Restore Finished**.

Normal Level Sequence playback nodes remain available.

## Important behavior

- Saved playback restores the correct position and preserves paused, stopped, or finished states.
- Runtime loop counts and completed-loop progress are preserved across saves.
- A sequence can wait for actors or bindings in streamed-out areas. If bindings do not become ready before the timeout, available playback state is still restored.
- A save made while restoration is pending does not replace the previously loaded playback state.
- Capture and restoration are server-authoritative. Replication is the project's responsibility.
