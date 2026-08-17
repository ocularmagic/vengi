/**
 * @file
 * Single output-transform contract for the voxel path tracer.
 *
 * Both the CPU voxel DDA tracer and the WebGPU/WGSL twin accumulate *linear*
 * radiance into the same accumulation buffer. `VoxelDDAPathTracer::image()` is
 * the one place that converts that linear HDR radiance to the displayed sRGB
 * bytes, and it does so through `pathTracerTonemap()` below. Keeping the whole
 * transform here (instead of calling yocto directly) makes the contract explicit
 * and lets a future WGSL mirror of this function stay in lock-step.
 *
 * Color-space contract (audited):
 *  - Palette/material albedo is sRGB bytes -> linear once via
 *    `paletteColorLinear()` (`color::srgbToLinear`).
 *  - Radiance is traced and accumulated in linear space (CPU and GPU agree).
 *  - This function applies, in order:
 *      1. exposure (stops, base-2) -> `rgb *= exp2(exposure)`
 *      2. filmic rolloff (ACES/Knarkowicz approximation) when enabled
 *      3. sRGB encode (linear -> sRGB)
 *  There is exactly one linear->sRGB conversion and no double/missing gamma.
 */

#pragma once

#include <glm/vec3.hpp>

namespace voxelpathtracer {

/**
 * ACES filmic approximation (Knarkowicz 2016). Maps linear HDR to [0, 1] with a
 * soft shoulder so bright highlights roll off instead of clipping to white.
 * Matches yocto::tonemap_filmic(..., false).
 */
inline glm::vec3 pathTracerFilmic(const glm::vec3 &linear) {
	const glm::vec3 hdr = linear * 0.6f;
	const glm::vec3 ldr = (hdr * hdr * 2.51f + hdr * 0.03f) / (hdr * hdr * 2.43f + hdr * 0.59f + 0.14f);
	return glm::max(ldr, glm::vec3(0.0f));
}

/**
 * Standard sRGB encode (IEC 61966-2-1), the exact inverse of the
 * `color::srgbToLinear` used for material input. Matches yocto::rgb_to_srgb.
 */
inline float pathTracerSrgbEncode(float linear) {
	linear = glm::max(linear, 0.0f);
	return linear <= 0.0031308f ? (12.92f * linear) : (1.055f * glm::pow(linear, 1.0f / 2.4f) - 0.055f);
}

/**
 * The one output transform: linear radiance -> display sRGB.
 *  - `exposure` is in stops (0 means no change).
 *  - `filmic` enables the ACES highlight rolloff.
 * Behavior-identical to yocto::tonemap(hdr, exposure, filmic, srgb = true).
 */
inline glm::vec3 pathTracerTonemap(const glm::vec3 &linearRadiance, float exposure, bool filmic) {
	glm::vec3 rgb = linearRadiance;
	if (exposure != 0.0f) {
		rgb *= glm::exp2(exposure);
	}
	if (filmic) {
		rgb = pathTracerFilmic(rgb);
	}
	rgb.x = pathTracerSrgbEncode(rgb.x);
	rgb.y = pathTracerSrgbEncode(rgb.y);
	rgb.z = pathTracerSrgbEncode(rgb.z);
	return rgb;
}

} // namespace voxelpathtracer
