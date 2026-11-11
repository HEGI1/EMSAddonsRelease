# Physics Streaming Guard

Use **EMS Physics Streaming Guard** to prevent a simulating actor from falling through missing geometry when its supporting streamed level or World Partition cell unloads.

## Basic use

1. Add an **EMS Physics Streaming Guard** component to an actor with simulating physics components.
2. No additional configuration is required.

**EMS Geometry Collection Actor** already includes this component.

## Blueprint nodes

- **Is Physics Suspended For Streaming** — reports whether the guard is currently holding the actor.

## Important behavior

- The guard acts when supporting streamed geometry is removed or is not yet available at startup. It does not freeze normal jumps, falls, or other gameplay movement.
- Support is checked under the whole footprint of each simulating component, so a wide body resting partly on a platform, or one resting across two cells, is held when any floor under it streams out.
- While held, the actor's simulating components have gravity disabled and their velocities cleared; the velocity is not restored afterwards. Physics stays enabled, so forces and collisions can still move a held actor.
- Gravity returns when support is back under the whole body, or when the world below has finished loading and still holds nothing up, so a body over a pit or past the map edge falls as usual. Support is rechecked every 0.1 seconds while held, so moving onto loaded ground also releases it.
- **Conventional level streaming:** sublevels have no location to check, so while any sublevel is not visible the body stays held. A guarded actor over a real void can then hang until every sublevel is visible. World Partition is checked by location and has no such limit.
- The guard only prevents falling. It does **not** save or restore actor state and does not handle replication.
- For [World Partition Placed Actors](WORLD_PARTITION_PLACED_ACTORS.md), author the guard on the actor class when it must also exist on a recreated actor.

## Settings

**EMS Physics Streaming Guard** has no settings.
