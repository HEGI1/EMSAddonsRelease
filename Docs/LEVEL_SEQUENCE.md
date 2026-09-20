# Level Sequence Persistence

Use **EMS Level Sequence Actor** to persist Level Sequence playback state through normal EMS saving.

Position, play rate, playback direction, and finished state restore together as one coherent playback state.

## Basic use

1. Place an **EMS Level Sequence Actor** instead of the standard Level Sequence Actor.
2. Assign the Level Sequence asset and configure playback normally.
3. Adjust restore options when needed.
4. Save and load through EMS normally.

The actor can live in streamed levels and World Partition cells like other placed actors.

## Restore options

- **Resume If Playing** — resumes a sequence that was active when saved. Disable it when gameplay should decide when playback continues.
- **Position Restore Policy** — **Jump And Evaluate** seeks directly to the saved position; **Traverse And Evaluate** evaluates the frames in between.
- **Restore Timeout** — limits how long restoration waits for the sequence player and bindings before falling back to authored autoplay.

## Blueprint nodes

- **Is EMS Addon Restore Complete** — checks whether deferred restoration has finished.
- **Get EMS Addon Restore Result** — returns the latest restore result.
- **Play Looping** — call this on the EMS Level Sequence Actor to preserve the requested loop count. Use `-1` for infinite playback, `0` for a single play, or a positive number for that many repeats.

Use the EMS Level Sequence Actor as the restore-status target. Normal playback still uses Unreal's Level Sequence nodes; use the actor's **Play Looping** wrapper when changing the loop count at runtime.

## Events

- **On Level Sequence Restore Started**
- **On Level Sequence Restore Finished**

## Important behavior

- Authored autoplay is held while EMS restoration is pending so it cannot race the saved state.
- Restore waits for root possessable bindings up to **Restore Timeout**.
- Saving while restoration is requested or pending preserves the loaded playback state until the player has been restored.
- Pausing and resuming preserves the runtime loop count and completed-loop progress, including after loading a paused sequence.
- Actors restored through Actor Spawner or World Partition Runtime Actors wait for EMS loading to finish without requiring membership in its loaded-actor event list.
- Authored autoplay survives streamed hide/show cycles.
- Capture and restore are server-authoritative. Replication remains the project's responsibility.
