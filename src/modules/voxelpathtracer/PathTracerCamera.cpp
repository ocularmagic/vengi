/**
 * @file
 */

#include "PathTracerCamera.h"
#include "video/Camera.h"
#include <glm/gtc/matrix_inverse.hpp>

namespace voxelpathtracer {

PathTracerCameraData pathTracerCameraData(const video::Camera &camera) {
	PathTracerCameraData data;
	data.inverseViewProjection = glm::inverse(camera.viewProjectionMatrix());
	const glm::ivec2 size = glm::max(camera.size(), glm::ivec2(1));
	data.viewport = glm::vec4((float)size.x, (float)size.y, 1.0f / (float)size.x, 1.0f / (float)size.y);
	return data;
}

math::Ray pathTracerCameraRay(const PathTracerCameraData &camera, float pixelX, float pixelY) {
	const float ndcX = pixelX * camera.viewport.z * 2.0f - 1.0f;
	const float ndcY = 1.0f - pixelY * camera.viewport.w * 2.0f;
	glm::vec4 nearPoint = camera.inverseViewProjection * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
	glm::vec4 farPoint = camera.inverseViewProjection * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
	nearPoint /= nearPoint.w;
	farPoint /= farPoint.w;
	const glm::vec3 origin(nearPoint);
	return math::Ray(origin, glm::normalize(glm::vec3(farPoint - nearPoint)));
}

} // namespace voxelpathtracer
