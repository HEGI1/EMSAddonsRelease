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
- While held, the actor's simulating components have gravity disabled and their velocities cleared. Their original gravity settings return when support is back under the whole body, or when the world below has finished loading and still holds nothing up. A body spawned over a pit or past the edge of the map therefore falls as usual.
- **Conventional level streaming:** a streamed sublevel has no location the guard can check. While any sublevel of the map is not visible, the guard cannot tell whether it holds the missing floor and keeps the body held. In such a map, a guarded actor spawned or left over a real void can stay in the air until every sublevel is visible. World Partition is checked by location and does not have this limit. Playtest this case if your game uses conventional streaming with guarded actors over open space.
- Physics simulation stays enabled. Forces and collisions can still move a held actor.
- The guard prevents falling; it does **not** save or restore actor state.
- For [World Partition Placed Actors](WORLD_PARTITION_PLACED_ACTORS.md), author the guard on the actor class when it must also exist on a recreated actor.
- Existing velocity is not restored after a hold. The guard works independently of EMS persistence and does not handle replication.

## Settings

**EMS Physics Streaming Guard** has no settings.
