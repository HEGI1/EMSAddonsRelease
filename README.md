# Easy Multi Save Addons

**Current Version: 0.5.2**

**Easy Multi Save Addons** provide ready-made extensions for more specialized Unreal Engine workflows that would otherwise require project-specific Blueprint logic. They build directly on Easy Multi Save, reusing its save system while keeping the core plugin lean and focused.

## Addons

| Addon | Use it for | Basic use |
| --- | --- | --- |
| **Autosave & Checkpoints** | Automatic saves and gameplay checkpoints. | Configure the settings, call **Request Autosave**, or place an **EMS Checkpoint**. |
| **Actor Spawner** | Runtime actors that must belong to a specific streamed level or World Partition cell. | Place an **EMS Actor Spawner** there and call **Spawn Actor**. |
| **World Partition Runtime Actors** | Runtime actors that should follow World Partition streaming based on their current location. | Call **Spawn World Partition Runtime Actor**. Nothing needs to be placed. |
| **Geometry Collection** | Saving fractured Chaos Geometry Collection state. | Use an **EMS Geometry Collection Actor**. |
| **Level Sequence** | Saving Level Sequence playback state. | Use an **EMS Level Sequence Actor**. |
| **Instance Manager** | Runtime changes to project-owned ISM and HISM instances and to painted Static Mesh Foliage. | Place one **EMS Instance Manager** in the persistent level. |
| **Physics Streaming Guard** | Keeping a simulating actor from falling out of the world while the geometry under it is streamed out. | Add an **EMS Physics Streaming Guard** component. **EMS Geometry Collection Actor** already has one. |

Save and load normally through EMS after setup.

### Runtime actor choice

- **Normal runtime actor:** use Easy Multi Save normally.
- **Must belong to a specific streamed level or cell:** use **Actor Spawner**.
- **Should unload and return with its current World Partition location:** use **World Partition Runtime Actors**.

## Documentation

- [Autosave and Checkpoints](Docs/AUTOSAVE_CHECKPOINTS.md)
- [Actor Spawner](Docs/ACTOR_SPAWNER.md)
- [World Partition Runtime Actors](Docs/WORLD_PARTITION_RUNTIME_ACTORS.md)
- [Geometry Collection Persistence](Docs/GEOMETRY_COLLECTION.md)
- [Level Sequence Persistence](Docs/LEVEL_SEQUENCE.md)
- [Instanced Meshes and Foliage](Docs/INSTANCED_MESHES_AND_FOLIAGE.md)
- [Physics Streaming Guard](Docs/PHYSICS_STREAMING_GUARD.md)

## Version history

See the [Changelog](Plugins/EmsAddons/CHANGELOG.md) for release notes and recent changes.

## Requirements
- EMSAddons 0.5.2
- Unreal Engine 5.8
- Easy Multi Save 1.85 or compatible newer release
- Unreal `GeometryCollectionPlugin`

Easy Multi Save is a separate dependency and is not included.

## Installation

Copy both plugins into the project:

```text
<Project>/Plugins/EasyMultiSave/EasyMultiSave.uplugin
<Project>/Plugins/EmsAddons/EmsAddons.uplugin
```

Enable the plugins, regenerate project files if required, and build the project.

For development from this repository, copy your licensed EMS installation to `Plugins/EasyMultiSave`, generate project files for `EMSAddonsDev.uproject`, and build the editor target. The repository uses Git LFS for binary assets.

## License

EMSAddons is source-available under the [EMSAddons License](Plugins/EmsAddons/LICENSE). Use requires authorization under a valid Easy Multi Save license, and EMS must remain a required dependency. Compiled EMSAddons code may be shipped as part of packaged games and applications. Standalone use or redistribution as a standalone product is not permitted.

Use of EMSAddons requires a valid Easy Multi Save license, including licenses obtained through Fab or the legacy Unreal Engine Marketplace, and successful verification of the applicable license entitlement through the official Easy Multi Save support Discord or another verification method designated by the copyright holder.

Easy Multi Save, Unreal Engine, and template content remain subject to their own licenses.
