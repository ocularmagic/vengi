/**
 * @file
 */

#include "PathTracerTraversal.h"
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>

namespace voxelpathtracer {

PathTracerRay pathTracerRay(const glm::vec3 &origin, const glm::vec3 &direction, float maximumDistance,
								int skipGrid, const glm::ivec3 &skipCell, bool shadowRay) {
	PathTracerRay ray;
	ray.originMin = glm::vec4(origin, 1.0e-4f);
	ray.directionMax = glm::vec4(direction, maximumDistance);
	ray.skipCellGrid = glm::ivec4(skipCell, skipGrid);
	ray.flags.x = shadowRay ? 1u : 0u;
	return ray;
}

bool pathTracerTraceGrid(const PathTracerScene &scene, uint32_t gridIndex, const PathTracerRay &ray,
						 PathTracerVoxelHit &hit) {
	if (gridIndex >= scene.grids.size()) {
		return false;
	}
	const PathTracerGrid &grid = scene.grids[gridIndex];
	const glm::ivec3 gridMins = grid.mins();
	const glm::ivec3 gridSize = grid.size();
	const glm::vec3 origin = ray.origin();
	const glm::vec3 worldDirection = ray.direction();
	const float worldDirectionLength2 = glm::dot(worldDirection, worldDirection);
	if (worldDirectionLength2 < 1.0e-16f) {
		return false;
	}
	const glm::vec3 normalizedWorldDirection = worldDirection / glm::sqrt(worldDirectionLength2);
	const glm::vec3 localOrigin = glm::vec3(grid.invWorldMat * glm::vec4(origin, 1.0f)) + grid.pivot();
	glm::vec3 localDirection = glm::mat3(grid.invWorldMat) * normalizedWorldDirection;
	const float localDirectionLength2 = glm::dot(localDirection, localDirection);
	if (localDirectionLength2 < 1.0e-16f) {
		return false;
	}
	localDirection /= glm::sqrt(localDirectionLength2);

	const glm::vec3 boundsMin(gridMins);
	const glm::vec3 boundsMax = boundsMin + glm::vec3(gridSize);
	float t0 = 0.0f;
	float t1 = 1.0e30f;
	int entryAxis = 0;
	for (int axis = 0; axis < 3; ++axis) {
		if (glm::abs(localDirection[axis]) < 1.0e-8f) {
			if (localOrigin[axis] < boundsMin[axis] || localOrigin[axis] >= boundsMax[axis]) {
				return false;
			}
			continue;
		}
		const float inverseDirection = 1.0f / localDirection[axis];
		float nearDistance = (boundsMin[axis] - localOrigin[axis]) * inverseDirection;
		float farDistance = (boundsMax[axis] - localOrigin[axis]) * inverseDirection;
		if (nearDistance > farDistance) {
			const float swap = nearDistance;
			nearDistance = farDistance;
			farDistance = swap;
		}
		if (nearDistance > t0) {
			t0 = nearDistance;
			entryAxis = axis;
		}
		t1 = glm::min(t1, farDistance);
		if (t0 > t1) {
			return false;
		}
	}
	if (t1 < 0.0f) {
		return false;
	}
	const bool startedInside = t0 <= 0.0f;
	if (startedInside) {
		t0 = 0.0f;
	}

	glm::vec3 samplePosition = localOrigin + localDirection * (t0 + 1.0e-4f);
	const glm::ivec3 cellMax = gridMins + gridSize - 1;
	glm::ivec3 cell(glm::clamp((int)glm::floor(samplePosition.x), gridMins.x, cellMax.x),
					glm::clamp((int)glm::floor(samplePosition.y), gridMins.y, cellMax.y),
					glm::clamp((int)glm::floor(samplePosition.z), gridMins.z, cellMax.z));
	const glm::ivec3 step(localDirection.x >= 0.0f ? 1 : -1, localDirection.y >= 0.0f ? 1 : -1,
						localDirection.z >= 0.0f ? 1 : -1);
	const glm::vec3 absoluteDirection = glm::abs(localDirection);
	const glm::vec3 delta(absoluteDirection.x < 1.0e-8f ? 1.0e30f : 1.0f / absoluteDirection.x,
					  absoluteDirection.y < 1.0e-8f ? 1.0e30f : 1.0f / absoluteDirection.y,
					  absoluteDirection.z < 1.0e-8f ? 1.0e30f : 1.0f / absoluteDirection.z);
	glm::vec3 next;
	next.x = absoluteDirection.x < 1.0e-8f
				 ? 1.0e30f
				 : ((localDirection.x >= 0.0f ? (float)(cell.x + 1) - samplePosition.x
											  : samplePosition.x - (float)cell.x) * delta.x);
	next.y = absoluteDirection.y < 1.0e-8f
				 ? 1.0e30f
				 : ((localDirection.y >= 0.0f ? (float)(cell.y + 1) - samplePosition.y
											  : samplePosition.y - (float)cell.y) * delta.y);
	next.z = absoluteDirection.z < 1.0e-8f
				 ? 1.0e30f
				 : ((localDirection.z >= 0.0f ? (float)(cell.z + 1) - samplePosition.z
											  : samplePosition.z - (float)cell.z) * delta.z);

	int lastAxis = entryAxis;
	float enterDistance = t0;
	const int maxSteps = gridSize.x + gridSize.y + gridSize.z + 8;
	for (int stepIndex = 0; stepIndex < maxSteps; ++stepIndex) {
		if (!grid.contains(cell)) {
			return false;
		}
		const uint32_t packed = scene.cell(grid, grid.localCellIndex(cell));
		if (packed != 0u) {
			const uint8_t materialIndex = (uint8_t)(packed - 1u);
			const uint8_t surfaceType = scene.material(grid, materialIndex).surfaceType();
			const bool skipOriginCell = (int)gridIndex == ray.skipGrid() && cell == ray.skipCell();
			const bool skipSurface = surfaceType == PathTracerSurfaceMedia ||
									 (ray.shadowRay() && !pathTracerSurfaceIsSolid(surfaceType));
			if (!skipOriginCell && !skipSurface) {
				glm::vec3 localPosition = localOrigin + localDirection * enterDistance;
				if (!startedInside || stepIndex > 0) {
					localPosition[lastAxis] =
						step[lastAxis] > 0 ? (float)cell[lastAxis] : (float)(cell[lastAxis] + 1);
				}
				const glm::vec3 worldPosition = glm::vec3(
					grid.worldMat * (glm::vec4(localPosition, 1.0f) - glm::vec4(grid.pivot(), 0.0f)));
				float worldDistance = glm::dot(worldPosition - origin, normalizedWorldDirection);
				if (worldDistance >= ray.directionMax.w) {
					return false;
				}
				worldDistance = glm::max(worldDistance, ray.originMin.w);
				glm::vec3 localNormal(0.0f);
				localNormal[lastAxis] = step[lastAxis] > 0 ? -1.0f : 1.0f;
				glm::vec3 worldNormal = glm::transpose(glm::mat3(grid.invWorldMat)) * localNormal;
				const float normalLength2 = glm::dot(worldNormal, worldNormal);
				worldNormal = normalLength2 > 1.0e-16f ? worldNormal / glm::sqrt(normalLength2)
														 : glm::vec3(0.0f, 1.0f, 0.0f);
				hit.positionT = glm::vec4(worldPosition, worldDistance);
				hit.normal = glm::vec4(worldNormal, 0.0f);
				hit.localPosition = glm::vec4(localPosition, 0.0f);
				hit.localNormal = glm::vec4(localNormal, 0.0f);
				hit.cellGrid = glm::ivec4(cell, (int)gridIndex);
				hit.data = glm::uvec4((uint32_t)materialIndex, 1u, 0u, 0u);
				return true;
			}
		}

		if (next.x < next.y) {
			if (next.x < next.z) {
				enterDistance = t0 + next.x;
				cell.x += step.x;
				lastAxis = 0;
				next.x += delta.x;
			} else {
				enterDistance = t0 + next.z;
				cell.z += step.z;
				lastAxis = 2;
				next.z += delta.z;
			}
		} else if (next.y < next.z) {
			enterDistance = t0 + next.y;
			cell.y += step.y;
			lastAxis = 1;
			next.y += delta.y;
		} else {
			enterDistance = t0 + next.z;
			cell.z += step.z;
			lastAxis = 2;
			next.z += delta.z;
		}
	}
	return false;
}

static PathTracerVoxelHit traceScene(const PathTracerScene &scene, const PathTracerRay &ray, uint32_t gridCount) {
	PathTracerVoxelHit closest;
	closest.positionT.w = ray.directionMax.w;
	for (uint32_t gridIndex = 0u; gridIndex < gridCount; ++gridIndex) {
		PathTracerRay limitedRay = ray;
		limitedRay.directionMax.w = closest.positionT.w;
		PathTracerVoxelHit candidate;
		if (pathTracerTraceGrid(scene, gridIndex, limitedRay, candidate) && candidate.positionT.w < closest.positionT.w) {
			closest = candidate;
		}
	}
	return closest;
}

PathTracerVoxelHit pathTracerTraceScene(const PathTracerScene &scene, const PathTracerRay &ray) {
	return traceScene(scene, ray, (uint32_t)scene.grids.size());
}

bool pathTracerTraceRays(const PathTracerScene &scene, const PathTracerRay *rays, PathTracerVoxelHit *hits,
						 const PathTracerDispatchParams &params) {
	if (params.gridCount > (uint32_t)scene.grids.size()) {
		return false;
	}
	if (params.rayCount == 0u) {
		return true;
	}
	if (rays == nullptr || hits == nullptr) {
		return false;
	}
	for (uint32_t rayIndex = 0u; rayIndex < params.rayCount; ++rayIndex) {
		hits[rayIndex] = traceScene(scene, rays[rayIndex], params.gridCount);
	}
	return true;
}

} // namespace voxelpathtracer
