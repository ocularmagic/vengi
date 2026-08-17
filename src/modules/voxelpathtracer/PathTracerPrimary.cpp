/**
 * @file
 */

#include "PathTracerPrimary.h"
#include "PathTracerSampling.h"

namespace voxelpathtracer {

PathTracerRay pathTracerPrimaryRay(const PathTracerCameraData &camera, const PathTracerPrimaryParams &params,
								   uint32_t pixelIndex) {
	if (pixelIndex >= params.pixelCount || camera.viewport.x < 1.0f || camera.viewport.y < 1.0f) {
		return PathTracerRay();
	}
	const uint32_t width = static_cast<uint32_t>(camera.viewport.x);
	const uint32_t x = pixelIndex % width;
	const uint32_t y = pixelIndex / width;
	const uint32_t pixelScramble = sampling::hash32(pixelIndex ^ 0xa511e9b3u);
	const glm::vec2 cameraSample = sampling::progressive2D(params.sampleIndex, pixelScramble);
	const math::Ray ray = pathTracerCameraRay(camera, static_cast<float>(x) + cameraSample.x,
										 static_cast<float>(y) + cameraSample.y);
	return pathTracerRay(ray.origin, ray.direction, 1.0e30f);
}

uint32_t pathTracerCopySampleOutputs(const PathTracerSampleOutput *outputs, uint32_t pixelCount, float *rgba,
									 float *albedo, float *normal, float *depth, float *luminanceSquared,
									 float *feature) {
	if (outputs == nullptr || pixelCount == 0u || rgba == nullptr || albedo == nullptr || normal == nullptr ||
		depth == nullptr || luminanceSquared == nullptr || feature == nullptr) {
		return 0u;
	}
	const float sampleValue = outputs[0].moments.y;
	if (sampleValue < 0.5f || sampleValue > 4294967040.0f) {
		return 0u;
	}
	const uint32_t sampleCount = static_cast<uint32_t>(sampleValue + 0.5f);
	for (uint32_t i = 0u; i < pixelCount; ++i) {
		if (glm::abs(outputs[i].moments.y - sampleValue) > 0.01f) {
			return 0u;
		}
	}
	for (uint32_t i = 0u; i < pixelCount; ++i) {
		const PathTracerSampleOutput &output = outputs[i];
		rgba[i * 4u + 0u] = output.radianceAlpha.x;
		rgba[i * 4u + 1u] = output.radianceAlpha.y;
		rgba[i * 4u + 2u] = output.radianceAlpha.z;
		rgba[i * 4u + 3u] = output.radianceAlpha.w;
		albedo[i * 3u + 0u] = output.albedoFeature.x;
		albedo[i * 3u + 1u] = output.albedoFeature.y;
		albedo[i * 3u + 2u] = output.albedoFeature.z;
		normal[i * 3u + 0u] = output.normalDepth.x;
		normal[i * 3u + 1u] = output.normalDepth.y;
		normal[i * 3u + 2u] = output.normalDepth.z;
		depth[i] = output.normalDepth.w;
		luminanceSquared[i] = output.moments.x;
		feature[i] = output.albedoFeature.w;
	}
	return sampleCount;
}

} // namespace voxelpathtracer
