/**
 * @file
 */

#pragma once

#include "PathTracerScene.h"
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <stddef.h>
#include <stdint.h>

namespace voxelpathtracer {

/**
 * GPU-aligned ray record shared with the future WGSL traversal kernel.
 */
struct alignas(16) PathTracerRay {
	// xyz world origin, minimum world distance
	glm::vec4 originMin{0.0f, 0.0f, 0.0f, 1.0e-4f};
	// xyz world direction, maximum world distance
	glm::vec4 directionMax{0.0f, 0.0f, -1.0f, 1.0e30f};
	// xyz cell to skip, grid index to skip (-1 means none)
	glm::ivec4 skipCellGrid{0, 0, 0, -1};
	// x: shadow ray, remaining values reserved
	glm::uvec4 flags{0u};

	inline glm::vec3 origin() const {
		return glm::vec3(originMin);
	}
	inline glm::vec3 direction() const {
		return glm::vec3(directionMax);
	}
	inline glm::ivec3 skipCell() const {
		return glm::ivec3(skipCellGrid);
	}
	inline int skipGrid() const {
		return skipCellGrid.w;
	}
	inline bool shadowRay() const {
		return flags.x != 0u;
	}
};

/**
 * GPU-aligned geometric hit. Shading data is fetched from the material buffer
 * through materialIndex after traversal.
 */
struct alignas(16) PathTracerVoxelHit {
	// xyz world position, world distance
	glm::vec4 positionT{0.0f, 0.0f, 0.0f, 1.0e30f};
	glm::vec4 normal{0.0f};
	glm::vec4 localPosition{0.0f};
	glm::vec4 localNormal{0.0f};
	// xyz voxel cell, grid index
	glm::ivec4 cellGrid{0, 0, 0, -1};
	// material index, hit flag, reserved, reserved
	glm::uvec4 data{0u};

	inline bool hit() const {
		return data.y != 0u;
	}
	inline uint8_t materialIndex() const {
		return (uint8_t)data.x;
	}
	inline int gridIndex() const {
		return cellGrid.w;
	}
	inline glm::ivec3 cell() const {
		return glm::ivec3(cellGrid);
	}
};

/**
 * Shared dispatch record for the CPU reference and WebGPU compute kernel.
 */
struct alignas(16) PathTracerDispatchParams {
	uint32_t rayCount = 0u;
	uint32_t gridCount = 0u;
	uint32_t reserved0 = 0u;
	uint32_t reserved1 = 0u;
};

static_assert(sizeof(PathTracerRay) == 64u, "PathTracerRay must match the WGSL record stride");
static_assert(alignof(PathTracerRay) == 16u, "PathTracerRay must remain 16-byte aligned");
static_assert(offsetof(PathTracerRay, directionMax) == 16u, "PathTracerRay direction offset changed");
static_assert(offsetof(PathTracerRay, skipCellGrid) == 32u, "PathTracerRay skip-cell offset changed");
static_assert(offsetof(PathTracerRay, flags) == 48u, "PathTracerRay flags offset changed");
static_assert(sizeof(PathTracerVoxelHit) == 96u, "PathTracerVoxelHit must match the WGSL record stride");
static_assert(alignof(PathTracerVoxelHit) == 16u, "PathTracerVoxelHit must remain 16-byte aligned");
static_assert(offsetof(PathTracerVoxelHit, normal) == 16u, "PathTracerVoxelHit normal offset changed");
static_assert(offsetof(PathTracerVoxelHit, localPosition) == 32u,
			  "PathTracerVoxelHit local position offset changed");
static_assert(offsetof(PathTracerVoxelHit, localNormal) == 48u,
			  "PathTracerVoxelHit local normal offset changed");
static_assert(offsetof(PathTracerVoxelHit, cellGrid) == 64u, "PathTracerVoxelHit cell offset changed");
static_assert(offsetof(PathTracerVoxelHit, data) == 80u, "PathTracerVoxelHit data offset changed");
static_assert(sizeof(PathTracerDispatchParams) == 16u,
			  "PathTracerDispatchParams must match the WGSL uniform size");
static_assert(alignof(PathTracerDispatchParams) == 16u,
			  "PathTracerDispatchParams must remain 16-byte aligned");

PathTracerRay pathTracerRay(const glm::vec3 &origin, const glm::vec3 &direction, float maximumDistance,
								int skipGrid = -1, const glm::ivec3 &skipCell = glm::ivec3(0), bool shadowRay = false);

bool pathTracerTraceGrid(const PathTracerScene &scene, uint32_t gridIndex, const PathTracerRay &ray,
						 PathTracerVoxelHit &hit);

PathTracerVoxelHit pathTracerTraceScene(const PathTracerScene &scene, const PathTracerRay &ray);

bool pathTracerTraceRays(const PathTracerScene &scene, const PathTracerRay *rays, PathTracerVoxelHit *hits,
						 const PathTracerDispatchParams &params);

} // namespace voxelpathtracer
