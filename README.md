# Easy Multi Save Addons

**Current Version: 1.3.2**

**Easy Multi Save Addons** add ready-made save workflows to Easy Multi Save (EMS) for cases that would otherwise need project-specific Blueprint logic. Each addon uses the normal EMS save system: set it up once, then save and load through EMS as usual.

## Addons

Pick the addon that matches your problem. Each one links to a short guide.

| Addon | Use it for | Start with |
| --- | --- | --- |
| [Autosave and Checkpoints](Docs/AUTOSAVE_CHECKPOINTS.md) | Autosaves, player-named saves, save menus, and checkpoints. | Call **Request Autosave** or **Manual Save**, or place an **EMS Checkpoint**. |
| [Actor Spawner](Docs/ACTOR_SPAWNER.md) | Runtime actors that belong to one streamed level or World Partition cell. | Place an **EMS Actor Spawner** there and call **Spawn Actor**. |
| [World Partition Placed Actors](Docs/WORLD_PARTITION_PLACED_ACTORS.md) | Placed actors that move around a World Partition map. | Add an **EMS World Partition Placed Component** to the placed actor. |
| [World Partition Runtime Actors](Docs/WORLD_PARTITION_RUNTIME_ACTORS.md) | Spawned actors that move around a World Partition map. | Call **Spawn World Partition Runtime Actor**. |
| [Geometry Collection](Docs/GEOMETRY_COLLECTION.md) | Fractured Chaos Geometry Collections. | Use an **EMS Geometry Collection Actor**. |
| [Level Sequence](Docs/LEVEL_SEQUENCE.md) | Level Sequence playback position and state. | Use an **EMS Level Sequence Actor**. |
| [Instanced Meshes and Foliage](Docs/INSTANCED_MESHES_AND_FOLIAGE.md) | Removed, moved, or replaced ISM/HISM instances and painted foliage. | Place one **EMS Instance Manager** in the persistent level. |
| [Physics Streaming Guard](Docs/PHYSICS_STREAMING_GUARD.md) | Stopping physics actors from falling when the ground under them streams out. | Add an **EMS Physics Streaming Guard** component. |

**Physics Streaming Guard** is a gameplay safeguard; it does not save anything itself.

## Requirements

- Unreal Engine 5.8
- Easy Multi Save 1.85 or newer (sold separately). With World Partition or level streaming, **Autosave and Checkpoints** needs EMS **1.86** or newer.
- Unreal's **Geometry Collection** plugin (enabled automatically)

## Installation

1. Copy both plugin folders into your project's `Plugins` folder:

   ```text
   <Project>/Plugins/EasyMultiSave/
   <Project>/Plugins/EmsAddons/
   ```

2. In **Edit → Plugins**, enable **Easy Multi Save** and **Easy Multi Save Addons**, then restart the editor if asked.

**Working from this repository:** copy your licensed EMS into `Plugins/EasyMultiSave/` and open `EMSAddonsDev.uproject`. Unreal offers to build the modules; if it doesn't, regenerate project files and build the editor target.

## Version history

See the [Changelog](Plugins/EmsAddons/CHANGELOG.md).

## License

EMSAddons is source-available under the [EMSAddons License](Plugins/EmsAddons/LICENSE). Use requires authorization under a valid Easy Multi Save license, and EMS must remain a required dependency. Compiled EMSAddons code may be shipped as part of packaged games and applications. Standalone use or redistribution as a standalone product is not permitted.

Use of EMSAddons requires a valid Easy Multi Save license, including licenses obtained through Fab or the legacy Unreal Engine Marketplace, and successful verification of the applicable license entitlement through the official Easy Multi Save support Discord or another verification method designated by the copyright holder.

Easy Multi Save, Unreal Engine, and template content remain subject to their own licenses.
