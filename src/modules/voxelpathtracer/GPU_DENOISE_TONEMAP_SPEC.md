# GPU denoise + tonemap pass — implementation spec

Status: **design (not implemented)**. Owner: `vengi-pathtracer` skill.
Follows renderer roadmap items 3 (adaptive) and 5 (SVGF denoiser), both already
committed. This pass is the deferred **performance** optimization called out at
the end of roadmap item 5: it is *not* a quality or parity change.

---

## 1. Goal and rationale (quantified)

The WebGPU tracer already produces the correct image. The problem is the
**readback**, not the compute. Measured on the real NVIDIA RTX 5070 at the
default 1280×720 (921,600 px), per batch:

| Operation | Time |
|---|---|
| `primaryMain` compute, 1 sample / 1 bounce | ~3.6 ms |
| Readback of `PathTracerSampleOutput` (96 B/px = 84.4 MiB) | ~42 ms |
| — of which `submit`+`map` | ~13.5 ms |
| — of which JS `slice()` + `HEAPU8.set` (two memcpys) | ~28 ms |

The GPU spends ~3.6 ms tracing; the CPU then spends ~42 ms shovelling guide
channels across the JS/wasm boundary every batch, **regardless of scene
complexity or denoise on/off**. At the default `batch=1` the readback is ~92%
of per-sample wall time. The fix is not "move compute to the GPU" (it is
already there and cheap) — it is **stop reading back the 96-byte guide struct**.

This spec moves the entire denoise (à-trous spatial + temporal) **and** the
tonemap into a WGSL compute pass, so the only thing read back is the final
display image.

Readback reduction:

| What is read back | bytes / px | vs 96 B/px |
|---|---|---|
| Full `PathTracerSampleOutput` (today) | 96 | 1× |
| Moments-only (`moments.xy`, for adaptive) | 8 | 12× |
| Final RGBA8 + 4-byte convergence flag (this spec) | **4** | **~24×** |

At 1280×720 the readback drops from ~42 ms to ~3–4 ms.

---

## 2. Non-goals / invariants that must NOT change

1. **Sampling stays bit-identical.** No change to the Sobol/Owen sampler, the
   scramble-constant map, `primaryRay`, `shadePrimary`, MIS, or Russian
   roulette. The `PathTracerSampleOutput` layout, `PathTracerPrimaryParams`
   (32-byte) layout, and the `moments` accumulation semantics are untouched.
   The scramble-constant map in the skill does **not** change this item.

2. **The denoiser and tonemap are NOT bit-parity targets.** They are
   floating-point post-processing (transcendentals: `exp2`, `pow`, rationals),
   unlike the integer-hash sampler. The requirement is *algorithmic identity*
   — same formula, same constants, same operation order — plus sane,
   visually identical output. This is the same "lock-step" language the
   output-transform contract uses; it is not the sampler's bit-exactness.

3. **The CPU path is the reference and stays as-is.** `VoxelDDAPathTracer::image()`
   + `denoiseColor()` + `denoiseTemporal()` + `pathTracerTonemap()` remain the
   desktop/CPU reference and the WebGPU fallback. The GPU pass is
   `__EMSCRIPTEN__`/WebGPU-only, exactly like every other GPU compute feature.

4. **No new UI.** Exposure, filmic, and denoise toggles already exist in the
   Render panel and already flow into `PathTracerState`. The pass reads them
   from the same place the CPU tonemap does.

---

## 3. Current pipeline (reference, for the port)

Readback: `dispatchPrimary` (JS) → `copyBufferToBuffer(sampleOutputs →
sampleReadback)` → `mapAsync(READ)` → `new Uint8Array(...).slice()` → stored
as `record.sampleResult` → `vengiPathTracerWebGPUTakePrimaryOutputs` →
`HEAPU8.set(record.sampleResult, outPtr)` (JS→wasm) → C++
`pathTracerCopySampleOutputs()` normalizes into `_accum` / `_accumAlbedo` /
`_accumNormal` / `_accumDepth` / `_accumLuma2` / `_accumFeature` /
`_accumCount`.

Denoise (`VoxelDDAPathTracer::denoiseColor`, lines 1769–1914):
1. Normalize accumulation per pixel by `1/max(_accumCount[i],1)`.
2. Derive mean albedo, normalized normal, alpha, depth, variance
   (`luma2·scale − luma²`), feature.
3. **Remove** the studio-bevel feature (`edgeFactor = clamp(feature,0.62,1.02)`,
   `src = rgb / edgeFactor`) so the analytic seam is not averaged away.
4. Three à-trous passes, step = 1,2,4, 3×3 kernel `[1,2,1]`, per-neighbor
   weight = `spatial · albedo · normal · depth · feature · featureBarrier ·
   alpha · color` where:
   - albedo `exp(−Δa²·80)`
   - normal `pow(ndot, 32)`
   - depth `exp(−(Δd/scale)²)`, `scale = 0.03·max(di,dj)+0.01`
   - feature `exp(−Δf²·1000)`; featureBarrier (step>1) `exp(−(1−minF)²·64)`
   - alpha `exp(−Δα²·64)`
   - color `exp(−0.125·(Δl/σ)²)`, `σ = 1.5·sqrt(vi+vj)+0.025·max(li,lj)+0.002`
5. **Re-apply** the feature (`rgb = src · edgeFactor`).
6. `denoiseTemporal` (lines 1916–2045): per-pixel world pos from
   `pathTracerCameraRay` + mean depth, reproject through
   `previousVP = inverse(_temporalCamera.inverseViewProjection)`; history
   rejection `normal pow(ndot,32) · depth exp(−Δ²) · albedo exp(−Δ²·80)`,
   reset when weight < 0.1; EMA `alpha = 1/(1+count)`, `count` capped 32;
   transparent/background pixels reset history.

Tonemap (`image()`, lines 2047–2080): normalize, optional `denoiseColor`, then
`pathTracerTonemap(c, exposure, filmic)` =
`exp2(exposure)` → ACES/Knarkowicz filmic (if enabled) → sRGB encode, alpha =
`clamp(accum[3]·scale)`, pack to RGBA8.

The WGSL mirror of every formula above must reproduce the **exact constants**
(`80`, `32`, `0.03/0.01`, `1000`, `64`, `0.125`, `1.5`, `0.025/0.002`,
`[1,2,1]`, ACES `2.51/0.03/2.43/0.59/0.14`, sRGB `0.0031308/12.92/1.055/2.4`).

---

## 4. Proposed GPU pipeline

Three compute entry points added to `PathTracerTraversalWGSL.h`, dispatched
from the JS driver. Buffers stay GPU-resident; nothing is read back except the
final image and a 4-byte flag.

### 4.1 `denoiseMain` — normalize + à-trous (per pixel)

`@compute @workgroup_size(64)`, one invocation per pixel.

- Reads `sampleOutputs[i]` (cumulative `PathTracerSampleOutput`).
- `count = max(moments.y, 1)`; normalize radiance/albedo/normal/alpha/depth by
  `1/count`; variance `= max(moments.x/count − mean², 0)`.
- 3 à-trous passes over a scratch ping-pong `storage, read_write` float buffer
  (`denoiseScratch`, 3×RGBA floats/px). Because each pass needs the previous
  pass's output (a stencil), it is **3 separate dispatches** (or one entry
  point parameterized by `step` via a uniform and dispatched 3×, which is
  cheaper to validate). Recommended: one entry point taking `step` in the
  `denoiseParams` uniform, dispatched 3×.
- Faithful port of the CPU weight chain, including the feature remove/re-apply
  around the passes.

### 4.2 `temporalMain` — history reprojection + blend (per pixel)

- Persistent GPU history buffers: `temporalColor` (3×f32/px), `temporalNormal`,
  `temporalAlbedo`, `temporalDepth`, `temporalCount`.
- `denoiseParams` carries `previousViewProjection` (4×f32×4, 64 bytes) computed
  on the C++ side as `inverse(_temporalCamera.inverseViewProjection)`; identity
  while the camera is fixed during accumulation.
- Faithful port of `denoiseTemporal` (lines 1964–2044): reject/reset on
  weight < 0.1, EMA `alpha = 1/(1+count)`, count cap 32, transparent reset.
- On first frame / restart, seed history from the spatially-filtered result and
  return it unchanged (mirror the `seed` branch).

### 4.3 `tonemapMain` — final RGBA8 (per pixel)

- Reads the denoised color (from `denoiseScratch` final ping buffer, or from
  `temporalColor` after `temporalMain`), applies
  `pathTracerTonemap(color, exposure, filmic)`, packs RGBA8 into `finalImage`
  (`storage, write`, 4 B/px). Alpha = `clamp(radianceAlpha.w / count)`.
- This is the **only** buffer copied to the map-read staging buffer.

### 4.4 Adaptive termination without the moments readback

Today `allPixelsConverged()` runs on the CPU from `_accumCount`/`_accumLuma2`,
which only exist because the full 96-byte struct is read back. To eliminate
that, add a `convergence` `storage` buffer (4 bytes) with an atomic counter.

**IMPLEMENTED (step 3, commit `be760ac`) as a separate `convergenceMain` entry
point** rather than an atomic inside `primaryMain` — this leaves the primary
bind-group layout untouched, gives exact post-batch semantics (no
one-extra-batch), and is a cleanly testable unit. It reuses `pixelConverged` so
the flag and the `primaryMain` early-out agree exactly:

- `convergenceMain` reads the cumulative `sampleOutputs`, checks the same
  `pixelConverged` predicate as `primaryMain`, and `atomicAdd`s one u32
  (`unconvergedCount`) for each still-unconverged pixel.
- The JS driver zeroes `unconvergedCount` before each check and reads the
  single u32 back (4 B) alongside `finalImage`.
- CPU reads `unconvergedCount == 0` instead of iterating `_accumCount`.

This replaces the per-pixel `moments` readback with a 4-byte read. (Fallback if
the atomic is finicky on some driver: read back only `moments.xy` = 8 B/px, an
already-12× reduction, and keep CPU-side `allPixelsConverged`.)

---

## 5. JS driver changes (`PathTracerWebGPU.cpp`, EM_JS block)

1. New buffers in the `record` (extend `dropBuffers` to destroy them on
   abort/recover): `denoiseScratch` (ping-pong, `storage, read_write`),
   `temporalColor/Normal/Albedo/Depth/Count` (`storage, read_write`),
   `finalImage` (`storage, write`), `convergence` (`storage, read_write`, 4 B),
   `denoiseParams` (`uniform`).
2. New pipelines for the three entry points (same `module`, `layout:'auto'`).
3. New `EM_JS` entry point `vengiPathTracerWebGPUDenoise(...)` that:
   - encodes the à-trous dispatches (3×) + temporal + tonemap,
   - `copyBufferToBuffer(finalImage → staging)`,
   - `copyBufferToBuffer(convergence → 4-byte staging)` (or map `convergence`
     directly),
   - submits, maps, returns `{ image: Uint8Array, unconverged: u32 }`.
4. `dispatchPrimary` keeps the accumulation-only path but **stops** copying
   `sampleOutputs → sampleReadback` (no more 96-byte readback). The existing
   `takePrimaryOutputs` / `pathTracerCopySampleOutputs` readback becomes
   unused in the WebGPU path (kept for the CPU fallback).

When `denoise` is off, still run `tonemapMain` (always) and skip the denoise
dispatches — mirror `image()`.

---

## 6. C++ changes (`VoxelDDAPathTracer.cpp`)

- WebGPU `update()` loop: after accumulation completes, call the new denoise
  EM_JS entry point instead of `takePrimaryOutputs` + `pathTracerCopySampleOutputs`.
- Store the returned RGBA8 in a member (`_webGPUFinalImage`) so `image()` can
  return it directly (skip CPU denoise/tonemap for the WebGPU path).
- Adaptive: read `unconverged` and use it for the `allPixelsConverged()`
  dispatch gate (`_sample < target && unconverged > 0`).
- `image()`: if the WebGPU final image is available, wrap it directly (no
  `denoiseColor`/`pathTracerTonemap` on the CPU copy). CPU path unchanged.

---

## 7. Lock-step contract (what must match, and to what degree)

| Piece | parity bar | notes |
|---|---|---|
| Sobol/Owen sampler, scramble map | **bit-exact** | untouched this item |
| `PathTracerSampleOutput` / `PrimaryParams` layout | **bit-exact** | untouched |
| Tonemap (`pathTracerTonemap` WGSL mirror) | formula-identical (float ulp-tolerant) | `exp2`/`pow` may differ in last ulp |
| à-trous + temporal denoise | algorithm-identical (heuristic) | same constants/order; not bit-pinned |

Update `references/output-transform.md` to note the WGSL tonemap mirror exists
and is formula-locked (not bit-locked), so a future editor does not "fix" a
last-ulp mismatch and break the contract's intent.

---

## 8. Pitfalls

- **Do not add bindings to `primaryMain`'s group by reindexing.** The denoise
  entry points get their own bind group layout (`layout:'auto'`), so their
  binding indices are independent; do not shift bindings 0–15 of the primary
  pipeline.
- **`moments.y` is the authoritative per-pixel count**, including under
  adaptive sampling. Normalize by `max(moments.y,1)`, never by
  `primaryParams.sampleIndex`.
- **Feature remove/re-apply must bracket the à-trous passes** exactly as CPU
  does, or the studio seam is averaged away (the CPU comment at line 1822).
- **Transparent/background pixels reset temporal history** (CPU line 1970);
  the GPU temporal pass must do the same or ghosting appears on alpha edges.
- **Reset GPU history on `restart()`/scene change**, matching the CPU's
  `_temporalValid=false` path. New scene already destroys `sampleOutputs`;
  extend that destroy to the temporal + final buffers.
- **`recover()`** (device loss) must also drop and re-create the new buffers
  (they are in `dropBuffers`) and re-seed history.
- **Staging/map buffer sizing**: `finalImage` is 4 B/px, so the map-read
  staging is ~4 MiB at 1280×720 (vs 84 MiB today) — well under WebGPU's
  256 MB buffer cap, no partial-mapping workaround needed.
- **Hard-refresh after rebuild** (Ctrl+Shift+R) and serve via
  `contrib/installer/emscripten/server.py` (COOP/COEP), per the skill.

---

## 9. Test plan

1. **CPU gtest (unchanged reference):** existing `testTonemapContract`,
   `testVoxelDDATemporalDenoiseConverges`, `testVoxelDDATemporalDenoiseResetsOnRestart`,
   adaptive tests — all must stay green (they pin the CPU reference the WGSL
   mirrors).
2. **WGSL tonemap lock-step:** add a JS twin of `pathTracerTonemap` /
   `pathTracerFilmic` / `pathTracerSrgbEncode` to the harness
   (`tests/webgpu/PathTracerTraversalWebGPU.html`) and assert the WGSL
   `tonemapMain` output matches the twin within a small float tolerance
   (≤ 1e-5 relative), mirroring how the sampler twin already works.
3. **WGSL denoise sanity:** harness dispatches `denoiseMain`+`temporalMain` on a
   small synthetic `sampleOutputs` buffer and asserts finite output, range
   `[0, 1]`-bounded final RGBA8, and that a flat (zero-variance) input is
   unchanged within tolerance.
4. **Convergence flag:** harness asserts `unconvergedCount` reaches 0 for a
   trivial scene after enough samples, and is > 0 before convergence.
5. **End-to-end regression:** `run_second_start.mjs` second-start check still
   logs `WebGPU path tracer active`, and a render produces an image identical
   (visually) to the CPU path.

Stop condition: readback of a 1280×720 batch drops from ~42 ms to ≤ ~5 ms
(measurable via the same `performance.now()` harness used for the profile),
CPU gtests green, WebGPU harness green, and CPU-vs-GPU final images match
within denoise noise.

---

## 10. Ordering / dependencies

1. `denoiseMain` + `tonemapMain` (no temporal yet) → proves the slim readback
   and the tonemap lock-step; spatial-only denoise first.
2. `temporalMain` + history buffers (completes roadmap item 5 on the GPU).
3. Convergence atomic (completes the adaptive readback elimination).
4. Wire into `VoxelDDAPathTracer` WebGPU loop + `image()`.

Each step is independently testable and commit-sized.
