# World Partition Placed Actors

Use **EMS World Partition Placed Component** for actors placed in the editor that move around a World Partition map, such as vehicles or pushable objects. They unload when their current area streams out and return, with their saved state, when it streams back in.

## Quick start

1. Add the **EMS Actor Save Interface** to the actor class.
2. Place the actor in the **main** World Partition map.
3. Add one **EMS World Partition Placed Component** to the class or the placed actor in the editor.
4. Save the map, then save and load normally through EMS.

The component turns off the actor's **Is Spatially Loaded** setting automatically; leave it off. Adding the component at runtime has no effect.

## Good to know

- Actors move freely between cells and keep their identity across streaming and loads.
- A character or physics actor also unloads when the floor under it streams out.
- Destroying an actor during gameplay is permanent. Loading an earlier save brings it back if it was alive then.
- Link actors with **soft references** or saved IDs. Hard references and attachments aren't reconnected after recreation.
- Only saved variables survive recreation. Mark per-instance values for EMS saving; extra components added only on the placed instance are not rebuilt.
- Add an [EMS Physics Streaming Guard](PHYSICS_STREAMING_GUARD.md) in the actor's class if it needs one after recreation.
- In **End Play**, use **Is Managed Actor Removal In Progress** to tell streaming removal apart from gameplay destruction.
- Up to 4,096 placed actors per map, including destroyed ones.
- Restoration runs on the server or in standalone. Replication is your project's responsibility.

**Not supported:** actors in Level Instances, conventional streamed sublevels, Runtime Data Layers, or HLODs.

## Multiple maps and older saves

Each map keeps its own actors. To keep several World Partition maps in one save slot, set EMS **Multi-Level Saving** to **Basic** or **Full**. With **Disabled** or **Streaming**, EMS keeps only the current map's data.

**Saves from before 1.3.0** don't restore these actors: they return to their editor-placed state, including ones the player destroyed, and a warning is logged. Save again to write data in the new format. A save with no data for an actor also uses its editor-placed state.

## Blueprint nodes

Use **Get World Subsystem** → **EMS World Partition Placed Subsystem** for the queries. **Get Placement Id** is on the component.

| Task | Nodes |
| --- | --- |
| Check an actor | **Is Placed Actor Managed**, **Is Managed Actor Removal In Progress** |
| List loaded actors | **Get Managed Placed Actors** |
| Read the actor's saved ID | **Get Placement Id** |

The component has no settings.
