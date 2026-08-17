# WebGPU traversal check

`PathTracerTraversalWebGPU.html` runs the production WGSL traversal in a real
browser WebGPU device. It uploads buffers with the same byte layout as the C++
`PathTracerGrid`, `PathTracerMaterial`, `PathTracerRay`, and
`PathTracerVoxelHit` records and verifies GPU readback for:

- a regular glass hit;
- a shadow ray that skips glass and reaches an opaque voxel;
- an origin-cell skip followed by a hit in the next voxel;
- a miss and its sentinel values.

It also dispatches the `primaryMain` entry point for a 2 by 2 camera. Those
checks cover the shared camera layout, progressive R2 pixel jitter, primary-ray
generation, traversal, and first-hit positions for all four pixels.
The same dispatch also validates finite one-sample environment lighting and the
albedo, analytic feature, normal, depth, position, opacity, and identity guide
channels needed for progressive accumulation and edge-preserving denoising.
It dispatches additional progressive samples as one aligned-uniform GPU batch
into the same output buffer and verifies the radiance/guide sums,
luminance-squared moment, and sample count.
It then destroys that device, requests a new one, and repeats a 64-sample /
8-bounce accumulation so a second start after teardown still returns a uniform
`moments.y` count.
A later case destroys the device while a primary readback is in flight, requests
a replacement device, and proves recovery still returns a uniform sample count.
A second forced loss must surface the CPU fallback message instead of a silent
black frame. Same-size storage/readback buffers are reused.
Finally, a diffuse floor under an emissive ceiling verifies that enabling a
second path bounce transports indirect emissive energy through the GGX/diffuse
mixture sampler.
Separate alpha and glass scenes verify that transparent paths continue into a
later emissive voxel, while a one-bounce path cannot leak that later hit.
The same floor/ceiling scene uploads a flat world-space emitter face and proves
that next-event sampling contributes direct light even with one path bounce.
An analytic ground-only camera ray verifies its shared bounds, material, hit
identity, depth/normal guides, and environment response.
A media-only ray verifies bounded volume emission/extinction and volume guide
channels. A synthetic floating-point HDRI verifies lat-long lookup, intensity,
and the uploaded luminance distribution contract.

From the repository root, serve the worktree over localhost:

```sh
python -m http.server 8765 --bind 127.0.0.1
```

Then open:

```text
http://127.0.0.1:8765/src/modules/voxelpathtracer/tests/webgpu/PathTracerTraversalWebGPU.html
```

The page reports `PASSED` only after WGSL compilation, both pipeline creations,
compute dispatches, readbacks, and all result checks succeed. Localhost
is required because WebGPU is available only in a secure browser context. The
test reads the shader directly from `PathTracerTraversalWGSL.h`, so it cannot
silently exercise a stale copy.

To wait for the async result from a headless Chrome with WebGPU enabled:

```sh
node src/modules/voxelpathtracer/tests/webgpu/run_second_start.mjs
```
