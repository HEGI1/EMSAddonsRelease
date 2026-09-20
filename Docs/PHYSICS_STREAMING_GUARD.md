# Physics Streaming Guard

Use **EMS Physics Streaming Guard** to keep a simulating actor from falling out
of the world while the geometry underneath it is streamed out.

## The problem

An actor resting on a floor that lives in a streamed level or a World Partition
cell keeps falling the moment that level is hidden. The floor's collision is
gone, gravity is not. By the time the cell streams back in, the actor is far
below the world — and a save taken meanwhile records it there. The same happens
at level start, where an actor can begin play a few frames before the cell under
it becomes visible.

## Basic use

1. Add an **EMS Physics Streaming Guard** component to any actor with simulating
   primitive components.
2. Nothing to configure.

**EMS Geometry Collection Actor** already carries one, so a placed Geometry
Collection needs no setup.

## Blueprint nodes

- **Is Physics Suspended For Streaming** — true while the guard is holding the
  owner because the world below it is unloaded.

## Important behavior

- The guard suspends only in two situations: the level that is actually holding
  the owner up is about to become invisible, or nothing at all is loaded below
  the owner at **Begin Play** in a world that streams (World Partition
  streaming, or a conventional streamed sublevel). A static world has nothing
  to explain missing support with, so it never runs the Begin Play check.
  Ordinary gameplay — a jump, an object knocked off a ledge, debris falling
  into a pit — reaches neither condition and is never frozen.
- It resumes as soon as a level becomes visible again and anything is loaded
  under the owner, including geometry further down than the original floor.
  Falling onto that is ordinary physics.
- Suspending clears velocity and turns gravity off for every simulating primitive
  on the owner; resuming hands the gravity setting back exactly as it was. An
  actor hidden while suspended gets it back at **End Play** too.
- Simulation itself is never switched off. On a Geometry Collection that would
  recreate the component's physics state and drop the Chaos proxy that Geometry
  Collection restore depends on. Later impulses, forces, or moving-body contacts
  can still move a suspended body; the guard does not lock its transform.
- This is a gameplay safeguard, not persistence. It runs in every net mode, it is
  not server-authoritative, and it never reads or writes save data.
- It reacts to streaming events rather than polling, so an idle guard costs
  nothing per frame.
