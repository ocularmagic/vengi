# Renderer

![voxedit-rendersettings](../../img/voxedit-rendersettings.png)

VoxEdit has built-in support for a voxel-grid path tracer (no mesh) plus the older Yocto mesh tracer - see [material](../../Material.md) docs for details. The Render panel starts the voxel-grid tracer. Desktop builds use the CPU implementation. Browser builds progressively render through WebGPU when it is available. If the GPU device is lost or runs out of memory, the tracer recovers once or continues on the CPU and shows that message in the Render panel instead of a silent black frame.

Open the **Render** panel and use the **Settings** menu in its menubar to configure the pathtracer. Settings are grouped into Presets, Quality, Output, Camera, Lighting, and Advanced. Start and stop the pathtracer from the same menubar.

The default lighting is a neutral Studio wrap (same light gray as the edit viewport). **Settings → Presets → Studio** restores that look. Enable **Sky environment** if you want the older blue sun-and-sky.

**Settings → Lighting → HDRI image** lights the scene from a Radiance `.hdr` environment map. Pick a file, then use **HDRI intensity** and **HDRI azimuth** to scale and rotate it. HDRI wins over sky and studio wrap; those sliders are disabled while HDRI is on. A failed load falls back to studio wrap.

**Hide environment** is on by default. Camera rays that miss the model (and the ground plane) write transparent pixels, so the backdrop is a void. Environment lighting, including HDRI, still reaches the voxels. Uncheck it to show the studio wrap, sky, or HDRI behind the model. The checkbox is stored on the scene.

**Settings → Output → Denoise** is a post-process on the current picture, not extra samples. After each display (and when you toggle the box) it averages grain on pixels that share a voxel face (same color and normal) and drops bright speckles. It does not add samples and does not restart the tracer. Cube edges stay 90 degrees. When the tracer finishes you will see **Done N / N** in the Render menubar; that is the last sample, not an early stop. The older Yocto mesh tracer only denoises if it was built with Open Image Denoise.

**Settings → Advanced → Ground plane** puts a light-grey floor under the lowest voxel so the model can cast shadows. **Voxel edges** adds a soft studio bevel on every exposed cube face.

Transparent and glass voxels filter light with Beer's law: palette alpha plus the **attenuation** slider stain and darken shadows (they are not a hard on/off cutout).

**Metal** (`metal` > 0, or Magica type Metal) is a GGX conductor. **Roughness** is the highlight width; **specular** scales the non-metal Fresnel. The default roughness of 0.1 on every swatch does not make a voxel shiny by itself.

**Volumetric** (palette checkbox; Magica type Media, or **density** > 0) is a participating volume, not a solid cube. **Density** is how quickly light is extinguished (mid-slider is still see-through; 1 is thick smoke). **Scatter** is how much environment light you see as mist (0 = smoke that only darkens, 1 = a visible cloud). **Rim light** is a bright edge toward the light; it needs Scatter. **Emit** on a volumetric color makes the volume itself glow (fire, plasma, neon). That glow is in the volume; it is not a solid lamp that paints the floor. Modest emit is a soft flame. **Boost emit** scales that (Magica flux) and is ignored when emit is 0.

HDRI path, intensity, rotation, ground plane, voxel edges, hide environment, exposure, and filmic output are stored on the scene when you save a `.vengi` file. They are not remembered as a global editor setting. Exposure, filmic output, and denoising update the accumulated image immediately without restarting the trace.

**Start path tracer** uses the camera from the last focused viewport tab (for example **Free EditMode**). It does **not** use a camera node you created unless you pick that node under **Settings → Camera**. After you move the viewport, use **Sync camera** to restart from the new view.

Creating a camera node in the Camera panel stores a snapshot. It does not keep the path tracer locked to whatever you are looking at now.
