# Voxelization

When converting mesh [formats](Formats.md) (OBJ, FBX, glTF, STL, PLY, etc.) to voxel formats, Vengi uses different voxelization algorithms to convert continuous triangle meshes into discrete voxel grids. The algorithm can be selected using the `voxformat_voxelizemode` configuration variable.

Textured meshes (glTF/GLB with a `baseColor` image, and other formats that loaded a texture) use **Solid** mode when `voxformat_voxelizemode` is left at `0`. Set the mode to `1` to force the old fast rasterizer.

## Solid Mode (textured mesh default)

**Mode:** `voxformat_voxelizemode=2` (also used automatically for textured imports when mode is `0`)

Occupancy and color are two separate passes.

1. **Occupancy:** mark voxels whose cell intersects the mesh. If `voxformat_fillhollow` is on, flood from the outside and keep the enclosed interior. This is a filled solid, not a shell painted with one fill color.
2. **Color:** every **surface** voxel uses the closest point on the original mesh from the voxel center, interpolates that triangle's UVs, and reads the albedo from a 2x2 neighborhood by picking the **highest-chroma** texel (not an average). That keeps painted colors and drops baked-shadow / AA fringe. glTF/GLB textures use the spec UV origin (upper-left).
3. **Palette:** only surface samples vote. Colors are grouped by hue (plus neutrals). Each group gets slots in proportion to how much of the surface it covers, then a gap-split median cut builds a **ramp** for that group. Voxels are remapped with HSB distance so a dusty sample snaps to its hue ramp instead of a nearby brown. Default target is 256 (`voxformat_targetcolors` `0`). Interior voxels are solid white and do not enter the histogram. Palette bytes are sRGB. glTF `baseColorFactor` is applied in linear space (skipped when the factor is 1) and encoded with the sRGB OETF so midtones are not crushed.

Cubic voxels do **not** store the source triangle normal. The edit viewport lights each cube from its visible face (same as MagicaVoxel `.vox`). Triangle normals on a cube shade the whole voxel as a slanted plane and look dark/grey. A normal palette is still attached to the node so you can calculate or paint normals later (Command & Conquer). Use **Show normals** to visualize stored normals; they are not used for cube lighting.

PNG slice export (`voxformat_imagesavetype` `0`) writes one XZ image per height (Y), top of the model first. Buried exact-white fill becomes transparent except a 1-voxel liner next to the colored shell. Disable the liner/hollow with `voxformat_imageslicehollowinterior false`.

Do not expect triangle-overlap averaging, largest-face heuristics, or `centerUV` fallbacks in this mode.

Size is unchanged from the rest of Vengi: `voxformat_voxelsize` (voxels on the longest axis) or `voxformat_scale`. VoxEdit does not have MagicaVoxel's 256-voxel-per-axis limit.

## High Quality Mode (Default)

**Mode:** `voxformat_voxelizemode=0`

This is the recommended voxelization mode that produces the most accurate and visually pleasing results.

### How It Works

1. **Triangle Subdivision:** Large triangles are recursively subdivided using a Sierpinski triangle algorithm. This ensures that even large mesh surfaces are properly sampled and converted to voxels with good coverage.

2. **Axis-Aligned Transformation:** Subdivided triangles are transformed and positioned precisely in the voxel grid using axis-aligned coordinates.

3. **Accurate Sampling:** Each voxel position is carefully evaluated to determine if it should be filled based on the mesh geometry.

### Visual Characteristics

- Accurate representation of the original mesh shape
- Properly handles thin surfaces and fine details
- Good preservation of texture colors and vertex colors
- Consistent results regardless of mesh complexity
- Smooth color sampling from textures and UV coordinates

### Performance Characteristics

- Slower than Fast mode due to triangle subdivision
- Uses more memory during processing (temporary subdivision data)
- Processing time increases significantly with mesh complexity
- Not recommended for extremely large meshes (> 512³ voxels)

### Best For

- Models where accuracy is important
- Detailed meshes with fine features
- Character models and organic shapes
- When texture color sampling is critical
- Models with thin surfaces that need proper representation

### Configuration Options

Additional settings that affect high quality voxelization:

| CVAR | Description | Default |
|------|-------------|---------|
| `voxformat_fillhollow` | Fill the interior of closed meshes | `true` |
| `voxformat_scale` | Uniformly scale the mesh before voxelization | `1.0` |
| `voxformat_scale_x/y/z` | Scale on specific axes | `1.0` |
| `voxformat_rgbweightedaverage` | Average colors based on triangle area contribution | `true` |
| `voxformat_rgbflattenfactor` | Flatten RGB colors when importing (0-255) | `1` |
| `voxformat_createpalette` | Generate palette from mesh colors vs. use existing | `true` |

## Fast Mode

**Mode:** `voxformat_voxelizemode=1`

A faster voxelization algorithm optimized for speed and memory efficiency. This mode skips triangle subdivision and directly voxelizes the mesh.

### How It Works

1. **Direct Voxelization:** Each triangle is directly rasterized into the voxel grid without subdivision.

2. **Per-Triangle Processing:** Colors are sampled directly from the triangle's UV coordinates. Cube voxels do not store interpolated triangle normals.

3. **Memory Efficient:** Doesn't create temporary subdivision data, keeping memory usage lower.

### Visual Characteristics

- Good results for small to medium triangles
- May have gaps or holes with very large triangles
- Faster color sampling
- Less accurate for meshes with large triangular faces
- May miss thin surfaces if triangles are too large

### Performance Characteristics

- Significantly faster than High Quality mode
- Lower memory usage during processing
- Better suited for large meshes
- Recommended for meshes larger than 512³ voxels

### Best For

- Very large meshes where memory is a concern
- When processing speed is more important than accuracy
- Meshes that already have reasonably sized triangles
- Batch processing of many files
- Preview/draft voxelizations

### Limitations

- Large triangles may not be fully filled
- Can produce gaps in the voxelization
- Less accurate sampling of texture data
- Not recommended for meshes with very large faces

### When to Use

The tool automatically suggests Fast mode when:
- Mesh dimensions exceed 512 voxels in any direction
- Memory is constrained
- Processing time needs to be minimized

You'll see a warning like this if using High Quality mode on large meshes:
```
Large meshes will take a lot of time and use a lot of memory. Consider scaling the mesh!
Another option when using very large meshes is to use the fast voxelization mode (voxformat_voxelizemode)
```

## Common Settings

These settings apply to both voxelization modes:

### Scaling

Control the size of the resulting voxel model:

```bash
# Uniformly scale to 2x size
voxconvert -set voxformat_scale 2.0 input.obj output.vox

# Scale differently on each axis
voxconvert -set voxformat_scale_x 2.0 -set voxformat_scale_y 1.0 -set voxformat_scale_z 0.5 input.obj output.vox
```

### Hollow Filling

By default, Vengi fills the interior of closed meshes. To create only surface voxels:

```bash
voxconvert -set voxformat_fillhollow false input.obj output.vox
```

### Color Handling

**Create Palette:** When enabled, automatically generates a palette from the mesh colors:
```bash
voxconvert -set voxformat_createpalette true input.obj output.vox
```

**Weighted Averaging:** When multiple triangles overlap a voxel, average colors based on triangle area:
```bash
voxconvert -set voxformat_rgbweightedaverage true input.obj output.vox
```

### Mesh Simplification

For very complex meshes, enable pre-voxelization simplification:

```bash
voxconvert -set voxformat_mesh_simplify true input.obj output.vox
```

## Choosing a Mode

Quick selection guide:

| Scenario | Recommended Mode | Reason |
|----------|------------------|--------|
| Textured glTF / GLB / OBJ | Solid (default for textures) | Nearest-surface UV, filled interiors |
| Detailed untextured models | High Quality | Preserves fine features and thin surfaces |
| Large terrain meshes | Fast | Better memory usage and performance |
| Architectural models | Solid or High Quality | Solid if textured; otherwise subdivision |
| Preview/draft work | Fast | Quick turnaround time |
| Mesh with large triangles | High Quality or Solid | Subdivision or occupancy, not the fast UV fallback |
| Batch processing | Fast | Faster overall processing time |
| Dimensions > 512³ | Fast or chunked | Necessary for memory constraints |

## Troubleshooting

### Model is too small/large after voxelization

Use the `voxformat_scale` settings to adjust size. See [FAQ](FAQ.md#my-model-is-very-smallbig-after-voxelization) for details.

### No colors after voxelization

Check texture paths and ensure `voxformat_createpalette` is enabled. See [FAQ](FAQ.md#no-colors-after-voxelization---whats-wrong) for details.

### Gaps or holes in voxelized mesh

- Switch to High Quality mode
- Check if `voxformat_fillhollow` is enabled
- Ensure mesh has proper watertight geometry

### Out of memory errors

- Use Fast mode instead of High Quality
- Scale the mesh down using `voxformat_scale`
- Simplify the mesh before voxelization

### Large GML/CityGML datasets

GML and CityGML files often contain large geographic datasets (entire city districts) that would result in enormous voxel regions. If the estimated voxel region after scaling exceeds 1024x256x1024 voxels, a warning is shown.

To limit the import to a specific area, use the `voxformat_gmlregion` cvar to specify a bounding region in GML world coordinates (the same coordinate system used in the source file):

```sh
voxconvert -set voxformat_gmlregion "548000 5930000 0 548500 5930500 100" --input input.gml --output output.vengi
```

Only objects whose geometry is **fully contained** within the specified region are imported. Objects that are partially or fully outside the region are skipped. The region filter is only applied when the estimated voxel size exceeds the threshold - for smaller datasets, all objects are imported regardless of the cvar value.

For more details on configuration, see [Configuration.md](Configuration.md).
