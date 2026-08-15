# Viewport

![voxedit-viewport](../../img/voxedit-viewport.png)

The viewport can get changed to [scene](SceneAndEditMode.md) and [edit](SceneAndEditMode.md) mode. You can switch the [cameras](Camera.md) from orthogonal to projection, you can record videos as `avi` or `mpeg` of your scene or let the [camera](Animations.md) automatically rotate by applying a small `omega` value for the rotation.

The canvas color is `ve_viewportcolor` (`r g b a`). Alpha `0` keeps the UI background. Studio view mode sets a light gray canvas.

`r_studiobevel` draws a soft rim on every voxel face. Combine it with cubic meshes and `voxel_mergequads=false` so same-color neighbors still read as individual cubes.
