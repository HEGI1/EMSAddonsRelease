# Physics Streaming Guard

Use **EMS Physics Streaming Guard** to stop a simulating physics actor from falling through the world when the level or World Partition cell under it streams out.

## Quick start

Add an **EMS Physics Streaming Guard** component to an actor with simulating physics. No setup is needed.

**EMS Geometry Collection Actor** already includes one.

## How it works

- When the ground under the actor streams out, or hasn't streamed in yet at startup, the guard turns off its gravity and stops it. Normal jumps and falls are not affected.
- The whole body is checked, so an actor resting partly on a platform or across two cells is held if any of its ground disappears.
- Gravity returns when ground is back under the actor, or when the area below has finished loading and there is genuinely nothing there, so actors over a real pit still fall.
- Velocity is not restored after release. Forces and collisions can still move a held actor.

## Good to know

- **Level streaming (not World Partition):** while any sublevel is hidden, the guard can't tell where it is, so held actors stay held until all sublevels are visible. World Partition doesn't have this limit.
- The guard only prevents falling. It doesn't save anything and works in every net mode.
- With [World Partition Placed Actors](WORLD_PARTITION_PLACED_ACTORS.md), add the guard in the actor's class (not just on the placed instance) so it exists after the actor is recreated.

## Blueprint nodes

| Task | Nodes |
| --- | --- |
| Check whether the actor is being held | **Is Physics Suspended For Streaming** |

The guard has no settings.
