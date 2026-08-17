/**
 * @file
 */

#pragma once

#include "PathTracerMaterial.h"
#include "core/collection/DynamicArray.h"
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <stddef.h>
#include <stdint.h>

namespace voxelpathtracer {

/**
 * Renderer-neutral grid descriptor. The field order and 16-byte records are
 * intentionally compatible with a WGSL storage-buffer struct.
 */
struct alignas(16) PathTracerGrid {
	glm::ivec4 minsData{0};
	glm::uvec4 sizeData{0u};
	// cell offset, material offset, reserved, reserved
	glm::uvec4 offsets{0u};
	glm::vec4 pivotData{0.0f};
	glm::mat4 worldMat{1.0f};
	glm::mat4 invWorldMat{1.0f};

	inline glm::ivec3 mins() const {
		return glm::ivec3(minsData);
	}
	inline glm::ivec3 size() const {
		return glm::ivec3(sizeData);
	}
	inline uint32_t cellOffset() const {
		return offsets.x;
	}
	inline uint32_t materialOffset() const {
		return offsets.y;
	}
	inline glm::vec3 pivot() const {
		return glm::vec3(pivotData);
	}
	inline uint32_t cellCount() const {
		return sizeData.x * sizeData.y * sizeData.z;
	}
	inline bool contains(const glm::ivec3 &cell) const {
		const glm::ivec3 lower = mins();
		const glm::ivec3 upper = lower + size();
		return cell.x >= lower.x && cell.y >= lower.y && cell.z >= lower.z && cell.x < upper.x &&
			   cell.y < upper.y && cell.z < upper.z;
	}
	inline uint32_t localCellIndex(const glm::ivec3 &cell) const {
		const glm::ivec3 local = cell - mins();
		return ((uint32_t)local.z * sizeData.y + (uint32_t)local.y) * sizeData.x + (uint32_t)local.x;
	}
};

static_assert(sizeof(PathTracerGrid) == 192u, "PathTracerGrid must match the WGSL record stride");
static_assert(alignof(PathTracerGrid) == 16u, "PathTracerGrid must remain 16-byte aligned");
static_assert(offsetof(PathTracerGrid, sizeData) == 16u, "PathTracerGrid size offset changed");
static_assert(offsetof(PathTracerGrid, offsets) == 32u, "PathTracerGrid buffer offsets changed");
static_assert(offsetof(PathTracerGrid, pivotData) == 48u, "PathTracerGrid pivot offset changed");
static_assert(offsetof(PathTracerGrid, worldMat) == 64u, "PathTracerGrid world matrix offset changed");
static_assert(offsetof(PathTracerGrid, invWorldMat) == 128u, "PathTracerGrid inverse matrix offset changed");

/**
 * World-space emissive voxel face shared by CPU and GPU next-event sampling.
 * edgeU and edgeV span the complete rectangular face from originArea.xyz.
 */
struct alignas(16) PathTracerEmitter {
	glm::vec4 originArea{0.0f};
	glm::vec4 edgeU{0.0f};
	glm::vec4 edgeV{0.0f};
	glm::vec4 normalData{0.0f};
	glm::vec4 emissionData{0.0f};
	// xyz voxel cell, w grid index
	glm::ivec4 cellGrid{0, 0, 0, -1};

	inline glm::vec3 origin() const {
		return glm::vec3(originArea);
	}
	inline float area() const {
		return originArea.w;
	}
	inline glm::vec3 normal() const {
		return glm::vec3(normalData);
	}
	inline glm::vec3 emission() const {
		return glm::vec3(emissionData);
	}
	inline glm::ivec3 cell() const {
		return glm::ivec3(cellGrid);
	}
	inline int gridIndex() const {
		return cellGrid.w;
	}
};

static_assert(sizeof(PathTracerEmitter) == 96u, "PathTracerEmitter must match the WGSL record stride");
static_assert(alignof(PathTracerEmitter) == 16u, "PathTracerEmitter must remain 16-byte aligned");
static_assert(offsetof(PathTracerEmitter, cellGrid) == 80u, "PathTracerEmitter identity offset changed");

/** Analytic finite ground plane shared by CPU and GPU transport. */
struct alignas(16) PathTracerGround {
	// minimum x, plane y, minimum z, enabled
	glm::vec4 boundsMin{0.0f};
	// maximum x, reserved, maximum z, reserved
	glm::vec4 boundsMax{0.0f};
	glm::vec4 albedoOpacity{0.82f, 0.82f, 0.84f, 1.0f};

	inline bool enabled() const {
		return boundsMin.w > 0.5f;
	}
};

static_assert(sizeof(PathTracerGround) == 48u, "PathTracerGround must match the WGSL uniform size");
static_assert(alignof(PathTracerGround) == 16u, "PathTracerGround must remain 16-byte aligned");

/**
 * Contiguous scene data consumed by the CPU path tracer and suitable for
 * direct upload into future WebGPU storage buffers.
 */
struct PathTracerScene {
	core::DynamicArray<PathTracerGrid> grids;
	core::DynamicArray<uint32_t> cells;
	core::DynamicArray<PathTracerMaterial> materials;
	core::DynamicArray<PathTracerEmitter> emitters;
	PathTracerGround ground;

	inline void clear() {
		grids.clear();
		cells.clear();
		materials.clear();
		emitters.clear();
		ground = PathTracerGround();
	}

	inline PathTracerGrid &addGrid(const glm::ivec3 &mins, const glm::ivec3 &size, const glm::mat4 &worldMat,
									 const glm::mat4 &invWorldMat, const glm::vec3 &pivot) {
		PathTracerGrid grid;
		grid.minsData = glm::ivec4(mins, 0);
		grid.sizeData = glm::uvec4(glm::uvec3(size), 0u);
		grid.offsets = glm::uvec4((uint32_t)cells.size(), (uint32_t)materials.size(), 0u, 0u);
		grid.pivotData = glm::vec4(pivot, 0.0f);
		grid.worldMat = worldMat;
		grid.invWorldMat = invWorldMat;
		cells.resize(cells.size() + grid.cellCount());
		materials.resize(materials.size() + 256u);
		grids.push_back(grid);
		return grids.back();
	}

	inline uint32_t &cell(const PathTracerGrid &grid, uint32_t localIndex) {
		return cells[grid.cellOffset() + localIndex];
	}
	inline uint32_t cell(const PathTracerGrid &grid, uint32_t localIndex) const {
		return cells[grid.cellOffset() + localIndex];
	}
	inline PathTracerMaterial &material(const PathTracerGrid &grid, uint8_t paletteIndex) {
		return materials[grid.materialOffset() + (uint32_t)paletteIndex];
	}
	inline const PathTracerMaterial &material(const PathTracerGrid &grid, uint8_t paletteIndex) const {
		return materials[grid.materialOffset() + (uint32_t)paletteIndex];
	}
};

} // namespace voxelpathtracer
