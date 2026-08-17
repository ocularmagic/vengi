/**
 * @file
 */

#pragma once

#include "math/Ray.h"
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <stddef.h>

namespace video {
class Camera;
}

namespace voxelpathtracer {

/**
 * Renderer camera snapshot. Its fields are 16-byte aligned types that map
 * directly to a WebGPU uniform buffer without carrying editor camera state.
 */
struct alignas(16) PathTracerCameraData {
	glm::mat4 inverseViewProjection{1.0f};
	// width, height, inverse width, inverse height
	glm::vec4 viewport{1.0f, 1.0f, 1.0f, 1.0f};
};

static_assert(sizeof(PathTracerCameraData) == 80u,
			  "PathTracerCameraData must match the WGSL uniform size");
static_assert(alignof(PathTracerCameraData) == 16u,
			  "PathTracerCameraData must remain 16-byte aligned");
static_assert(offsetof(PathTracerCameraData, viewport) == 64u,
			  "PathTracerCameraData viewport offset changed");

PathTracerCameraData pathTracerCameraData(const video::Camera &camera);
math::Ray pathTracerCameraRay(const PathTracerCameraData &camera, float pixelX, float pixelY);

} // namespace voxelpathtracer
