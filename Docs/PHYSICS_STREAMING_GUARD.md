# Physics Streaming Guard

Use **EMS Physics Streaming Guard** to stop an actor from falling through the world when the level or World Partition cell under it streams out, or hasn't streamed in yet.

## Quick start

**Players and AI characters:** nothing to do. Every pawn a player possesses, and every Character possessed by an AI controller, gets a guard automatically when it is possessed. You can turn either off in **Project Settings > Easy Multi Save Addons > Physics Streaming Guard**. Pawns nobody possesses, and pawns that already have a guard, are left as they are.

**Physics actors:** add an **EMS Physics Streaming Guard** component to an actor with simulating physics. No setup is needed.

**EMS Geometry Collection Actor** already includes one.

## How it works

- When the ground under the actor streams out, or hasn't streamed in yet at startup, the guard holds the actor in place. Normal jumps and falls are not continuously checked.
- After a teleport (an EMS load, a checkpoint restore, or a gameplay teleport) the guard checks again, so a player sent into an area that is still loading waits there instead of falling. This also applies to an actor already being held: the destination gets a fresh arrival check and grace period.
- Simulating bodies are held by turning off their gravity. Characters are held by setting their **Character Movement** to **None**: they can't move, jump or fall until the ground is there. Their previous movement mode is restored afterwards, and an AI's active **Move To** is paused and resumed rather than canceled.
- A character standing on a walkable floor counts as supported, even at a ledge where most of its capsule overhangs. The floor must be under the character where it is now: after a teleport, the floor it left behind doesn't count.
- An actor that appears or is teleported over a floor that is still loading is held even when other ground is loaded far below it, so it doesn't fall past the missing floor.
- Only collision that blocks the actor counts as ground, including vehicles, destructibles and custom collision object channels. Triggers and other overlap-only volumes are ignored.
- The whole body is checked, so an actor resting partly on a platform or across two cells is held if any of its ground disappears.
- The actor is released when ground is back under it, or when the area below has finished loading and there is genuinely nothing there, so actors over a real pit still fall.
- With World Partition, an actor is also held while any cell that isn't loaded yet has content below it. This covers large floors that reach beyond their own cell.
- Velocity is not restored after release. Forces and collisions can still move a held physics actor.

## Good to know

- **Multiplayer:** characters are held by the server (or in standalone), and the hold reaches clients through normal movement replication. Clients never add guards automatically. A client whose own cells load later than the server's may still see brief corrections, as in any Unreal project.
- **Level streaming (not World Partition):** hidden sublevels have no location the guard can check. An actor arriving over missing support waits while a sublevel is requested to become visible. If none is requested, an actor with loaded ground somewhere below falls onto it right away; one with nothing below is released after 10 seconds.
- **World Partition:** an arrival waits while missing content below it is being requested. If no such content is requested, the actor is released after 10 seconds, including when an unloaded cell at its location is outside the streaming sources' ranges. A hold that began because the floor streamed out has no time limit.
- **World Partition with sublevels:** both are checked. Once the cells below are loaded, a hidden sublevel that isn't requested is treated as in the level streaming case above.
- Physics Only collision is checked for simulating bodies when its level begins to hide (characters only stand on query collision). Arrival and release probes use Unreal's scene queries, which cannot see Physics Only collision; use Query and Physics for floors that must count as support in those checks.
- If gameplay changes a held character's movement mode (for example to Flying), the guard lets go and keeps your mode.
- Flying, swimming and custom movement modes are never held.
- The guard only prevents falling. It doesn't save anything.
- With [World Partition Placed Actors](WORLD_PARTITION_PLACED_ACTORS.md), add the guard in the actor's class (not just on the placed instance) so it exists after the actor is recreated.

## Blueprint nodes

| Task | Nodes |
| --- | --- |
| Check whether the actor is being held | **Is Physics Suspended For Streaming** |
| Check again after moving the actor without a teleport | **Reevaluate Streaming Support** |

## Project settings

| Setting | Default | What it does |
| --- | --- | --- |
| **Guard Player Pawns** | On | Adds a guard to every pawn a player possesses (characters and physics pawns such as vehicles). Spectators and other pawns without Character Movement or simulating physics are skipped. |
| **Guard AI Characters** | On | Adds a guard to every Character possessed by an AI controller. |
