/**
 * @file
 */

#pragma once

#include "PathTracerCamera.h"
#include "PathTracerTraversal.h"
#include <stdint.h>

namespace voxelpathtracer {

struct alignas(16) PathTracerPrimaryParams {
	uint32_t pixelCount = 0u;
	uint32_t sampleIndex = 0u;
	uint32_t gridCount = 0u;
	uint32_t emitterCount = 0u;
	// Adaptive sampling: 0 or 1 enables variance-based per-pixel early stop.
	uint32_t adaptiveEnabled = 0u;
	float adaptiveError = 0.02f;
	uint32_t adaptiveMinSamples = 16u;
	uint32_t reserved = 0u;
};

struct alignas(16) PathTracerLightingData {
	// rgb environment color, reserved
	glm::vec4 environmentColor{0.91f, 0.91f, 0.92f, 0.0f};
	// xyz sun direction, intensity
	glm::vec4 sunDirectionIntensity{0.0f, 1.0f, 0.0f, 1.0f};
	// HDRI intensity, HDRI azimuth, radiance clamp, reserved
	glm::vec4 environmentParams{1.0f, 0.0f, 10.0f, 0.0f};
	// environment mode (0 studio, 1 sky, 2 HDRI), hide environment,
	// studio edges, maximum path bounces
	glm::uvec4 flags{0u, 1u, 0u, 1u};
};

struct alignas(16) PathTracerEnvironmentData {
	// width, height, texel count, HDRI available
	glm::uvec4 dimensions{1u, 1u, 1u, 0u};
	// cumulative distribution sum, reserved, reserved, reserved
	glm::vec4 distribution{0.0f};
};

struct alignas(16) PathTracerMediaData {
	// enabled, maximum march steps, reserved, reserved
	glm::uvec4 flags{0u, 232u, 0u, 0u};
	// step length, maximum distance, reserved, reserved
	glm::vec4 params{0.28f, 64.0f, 0.0f, 0.0f};
};

/**
 * Progressive radiance plus the guide channels required by the edge-preserving
 * denoiser. GPU dispatches store running sums; moments.y is the authoritative
 * sample count used to normalize the other floating-point fields.
 */
struct alignas(16) PathTracerSampleOutput {
	glm::vec4 radianceAlpha{0.0f};
	// rgb albedo, analytic voxel feature factor
	glm::vec4 albedoFeature{0.0f, 0.0f, 0.0f, 1.0f};
	// xyz shading normal, depth
	glm::vec4 normalDepth{0.0f};
	// xyz world position, opacity
	glm::vec4 positionOpacity{0.0f};
	// material index, grid index, surface type, hit flag
	glm::uvec4 ids{0u};
	// luminance squared, accumulated sample count, reserved, reserved
	glm::vec4 moments{0.0f};
};

static_assert(sizeof(PathTracerPrimaryParams) == 32u,
			  "PathTracerPrimaryParams must match the WGSL uniform size");
static_assert(alignof(PathTracerPrimaryParams) == 16u,
			  "PathTracerPrimaryParams must remain 16-byte aligned");
static_assert(sizeof(PathTracerLightingData) == 64u,
			  "PathTracerLightingData must match the WGSL uniform size");
static_assert(alignof(PathTracerLightingData) == 16u,
			  "PathTracerLightingData must remain 16-byte aligned");
static_assert(sizeof(PathTracerEnvironmentData) == 32u,
			  "PathTracerEnvironmentData must match the WGSL uniform size");
static_assert(alignof(PathTracerEnvironmentData) == 16u,
			  "PathTracerEnvironmentData must remain 16-byte aligned");
static_assert(sizeof(PathTracerMediaData) == 32u,
			  "PathTracerMediaData must match the WGSL uniform size");
static_assert(alignof(PathTracerMediaData) == 16u,
			  "PathTracerMediaData must remain 16-byte aligned");
static_assert(sizeof(PathTracerSampleOutput) == 96u,
			  "PathTracerSampleOutput must match the WGSL record stride");
static_assert(alignof(PathTracerSampleOutput) == 16u,
			  "PathTracerSampleOutput must remain 16-byte aligned");

PathTracerRay pathTracerPrimaryRay(const PathTracerCameraData &camera, const PathTracerPrimaryParams &params,
								   uint32_t pixelIndex);

/** Copy cumulative GPU sample outputs into the CPU accumulation layout. Returns
 * the maximum per-pixel sample count (the global pass count) and writes each
 * pixel's own count into @p sampleCounts; returns 0 if any count is invalid. */
uint32_t pathTracerCopySampleOutputs(const PathTracerSampleOutput *outputs, uint32_t pixelCount, float *rgba,
									 float *albedo, float *normal, float *depth, float *luminanceSquared,
									 float *feature, int *sampleCounts);

} // namespace voxelpathtracer
