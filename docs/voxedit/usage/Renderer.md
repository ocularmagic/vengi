# Renderer

![voxedit-rendersettings](../../img/voxedit-rendersettings.png)

VoxEdit has built-in support for the yocto pathtracer - see [material](../../Material.md) docs for details.

Open the **Render** panel and use the **Settings** menu in its menubar to configure the pathtracer. Settings are grouped into Presets, Quality, Output, Camera, Lighting, and Advanced. Start and stop the pathtracer from the same menubar.

**Start path tracer** uses the camera from the last focused viewport tab (for example **Free EditMode**). It does **not** use a camera node you created unless you pick that node under **Settings → Camera**. After you move the viewport, use **Sync camera** to restart from the new view.

Creating a camera node in the Camera panel stores a snapshot. It does not keep the path tracer locked to whatever you are looking at now.
