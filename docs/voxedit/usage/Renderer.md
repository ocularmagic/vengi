# Renderer

![voxedit-rendersettings](../../img/voxedit-rendersettings.png)

VoxEdit has built-in support for the yocto pathtracer - see [material](../../Material.md) docs for details.

Open the **Render** panel and use the **Settings** menu in its menubar to configure the pathtracer. Settings are grouped into Presets, Quality, Output, Camera, Lighting, and Advanced. Start and stop the pathtracer from the same menubar.

The default lighting is a neutral Studio wrap (same light gray as the edit viewport). **Settings → Presets → Studio** restores that look. Enable **Sky environment** if you want the older blue sun-and-sky.

**Settings → Lighting → HDRI image** lights the scene from a Radiance `.hdr` environment map. Pick a file, then use **HDRI intensity** and **HDRI azimuth** to scale and rotate it. HDRI wins over sky and studio wrap; those sliders are disabled while HDRI is on. A failed load falls back to studio wrap. **Hide environment** still hides the backdrop while keeping the lighting.

**Settings → Advanced → Ground plane** puts a light-grey floor under the lowest voxel so the model can cast shadows. **Voxel edges** adds a soft studio bevel on every exposed cube face.

HDRI path, intensity, rotation, ground plane, and voxel edges are stored on the scene when you save a `.vengi` file. They are not remembered as a global editor setting.

**Start path tracer** uses the camera from the last focused viewport tab (for example **Free EditMode**). It does **not** use a camera node you created unless you pick that node under **Settings → Camera**. After you move the viewport, use **Sync camera** to restart from the new view.

Creating a camera node in the Camera panel stores a snapshot. It does not keep the path tracer locked to whatever you are looking at now.
