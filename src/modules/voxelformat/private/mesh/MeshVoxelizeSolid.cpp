/**
 * @file
 *
 * Occupancy (triangle overlap + optional interior fill) then color:
 * surface voxels sample the closest point on the mesh (Sample Nearest
 * Surface) and take the highest-chroma albedo texel in a 2x2 neighborhood.
 * Interior voxels are solid white and do not vote on the palette.
 */

#include "MeshFormat.h"
#include "app/ForParallel.h"
#include "core/Common.h"
#include "core/GLM.h"
#include "core/Log.h"
#include "core/collection/Buffer.h"
#include "core/collection/DynamicArray.h"
#include "core/concurrent/Atomic.h"
#include "palette/Palette.h"
#include "scenegraph/SceneGraphNode.h"
#include "voxel/RawVolume.h"
#include "voxel/Region.h"
#include "voxel/Voxel.h"
#include <float.h>
#include <glm/geometric.hpp>

namespace voxelformat {

namespace {

enum Occupancy : uint8_t { OccEmpty = 0, OccSurface = 1, OccInterior = 2, OccExterior = 3 };

struct SurfaceSample {
	int triIdx = -1;
	float distSq = FLT_MAX;
	glm::vec2 uv{0.0f};
};

void considerTriangle(const MeshTriCollection &tris, int triIdx, const glm::vec3 &center, SurfaceSample &best) {
	glm::vec3 hit;
	glm::vec2 uv;
	tris[triIdx].closestPoint(center, hit, uv);
	const glm::vec3 d = center - hit;
	const float dsq = glm::dot(d, d);
	if (dsq < best.distSq) {
		best.distSq = dsq;
		best.triIdx = triIdx;
		best.uv = uv;
	}
}

// Closest point on the mesh to a voxel center. Triangles are listed in every
// cell they intersect, so expanding Chebyshev rings around the voxel finds the
// same hit as Blender's Sample Nearest Surface without scanning 1.5M tris.
void sampleNearestSurface(const glm::ivec3 &pos, const glm::vec3 &center, const voxel::Region &region,
						  const MeshTriCollection &tris, const core::Buffer<int32_t> &head,
						  const core::DynamicArray<int32_t> &next, const core::DynamicArray<int32_t> &triOf,
						  SurfaceSample &best) {
	const glm::ivec3 rmin = region.getLowerCorner();
	const glm::ivec3 rmax = region.getUpperCorner();
	const int maxR = core_max(region.getWidthInVoxels(),
							  core_max(region.getHeightInVoxels(), region.getDepthInVoxels()));
	for (int r = 0; r <= maxR; ++r) {
		if (r > 0) {
			const float minDist = (float)r - 0.5f;
			if (minDist * minDist > best.distSq) {
				break;
			}
		}
		for (int z = pos.z - r; z <= pos.z + r; ++z) {
			for (int y = pos.y - r; y <= pos.y + r; ++y) {
				for (int x = pos.x - r; x <= pos.x + r; ++x) {
					const int dx = x > pos.x ? x - pos.x : pos.x - x;
					const int dy = y > pos.y ? y - pos.y : pos.y - y;
					const int dz = z > pos.z ? z - pos.z : pos.z - z;
					if (core_max(dx, core_max(dy, dz)) != r) {
						continue;
					}
					if (x < rmin.x || y < rmin.y || z < rmin.z || x > rmax.x || y > rmax.y || z > rmax.z) {
						continue;
					}
					for (int32_t node = head[region.index(x, y, z)]; node >= 0; node = next[node]) {
						considerTriangle(tris, triOf[node], center, best);
					}
				}
			}
		}
	}
}

} // namespace

void MeshFormat::voxelizeSolid(scenegraph::SceneGraphNode &node, const voxel::Region &region,
							   const MeshTriCollection &tris, const MeshMaterialArray &meshMaterialArray,
							   const palette::NormalPalette &normalPalette, bool fillHollow) const {
	if (tris.empty()) {
		Log::warn("Solid voxelize: no triangles");
		return;
	}

	const int64_t voxelCount = (int64_t)region.voxels();
	if (voxelCount <= 0) {
		Log::error("Solid voxelize: empty region");
		return;
	}

	Log::info("Solid voxelize: %i triangles, region %s (%i voxels), fillHollow=%s", (int)tris.size(),
			  region.toString().c_str(), (int)voxelCount, fillHollow ? "true" : "false");

	core::Buffer<uint8_t> occupancy((size_t)voxelCount);
	// Per-voxel linked list of overlapping triangle indices (not AABB-only).
	core::Buffer<int32_t> head((size_t)voxelCount, 0xFF);
	core::DynamicArray<int32_t> next;
	core::DynamicArray<int32_t> triOf;
	const int listReserve = (int)glm::min(voxelCount, (int64_t)tris.size() * 8);
	next.reserve(listReserve);
	triOf.reserve(listReserve);

	const glm::vec3 voxelHalf(0.5f);
	const glm::ivec3 rmin = region.getLowerCorner();
	const glm::ivec3 rmax = region.getUpperCorner();

	for (int triIdx = 0; triIdx < (int)tris.size(); ++triIdx) {
		if ((triIdx & 16383) == 0 && stopExecution()) {
			return;
		}
		const voxelformat::MeshTri &tri = tris[triIdx];
		const glm::vec3 &v0 = tri.vertex0();
		const glm::vec3 &v1 = tri.vertex1();
		const glm::vec3 &v2 = tri.vertex2();
		const glm::ivec3 imins = glm::max(glm::ivec3(glm::floor(tri.mins())), rmin);
		const glm::ivec3 imaxs = glm::min(glm::ivec3(glm::ceil(tri.maxs())), rmax);
		if (imins.x > imaxs.x || imins.y > imaxs.y || imins.z > imaxs.z) {
			continue;
		}
		const bool single = imins == imaxs;
		for (int z = imins.z; z <= imaxs.z; ++z) {
			for (int y = imins.y; y <= imaxs.y; ++y) {
				for (int x = imins.x; x <= imaxs.x; ++x) {
					if (!single) {
						const glm::vec3 center((float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f);
						if (!glm::intersectTriangleAABB(center, voxelHalf, v0, v1, v2)) {
							continue;
						}
					}
					const int64_t idx = region.index(x, y, z);
					occupancy[idx] = OccSurface;
					next.push_back(head[idx]);
					triOf.push_back(triIdx);
					head[idx] = (int32_t)(next.size() - 1);
				}
			}
		}
	}

	if (fillHollow) {
		core::DynamicArray<int64_t> queue;
		queue.reserve((int)glm::min(voxelCount / 8, (int64_t)262144));
		auto seed = [&](int x, int y, int z) {
			if (!region.containsPoint(x, y, z)) {
				return;
			}
			const int64_t idx = region.index(x, y, z);
			if (occupancy[idx] != OccEmpty) {
				return;
			}
			occupancy[idx] = OccExterior;
			queue.push_back(idx);
		};
		for (int y = rmin.y; y <= rmax.y; ++y) {
			for (int x = rmin.x; x <= rmax.x; ++x) {
				seed(x, y, rmin.z);
				seed(x, y, rmax.z);
			}
		}
		for (int z = rmin.z; z <= rmax.z; ++z) {
			for (int x = rmin.x; x <= rmax.x; ++x) {
				seed(x, rmin.y, z);
				seed(x, rmax.y, z);
			}
		}
		for (int z = rmin.z; z <= rmax.z; ++z) {
			for (int y = rmin.y; y <= rmax.y; ++y) {
				seed(rmin.x, y, z);
				seed(rmax.x, y, z);
			}
		}

		static const glm::ivec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
		int qhead = 0;
		while (qhead < (int)queue.size()) {
			if ((qhead & 65535) == 0 && stopExecution()) {
				return;
			}
			const glm::ivec3 p = region.fromIndex(queue[qhead++]);
			for (int i = 0; i < 6; ++i) {
				const glm::ivec3 n = p + dirs[i];
				if (!region.containsPoint(n)) {
					continue;
				}
				const int64_t idx = region.index(n);
				if (occupancy[idx] != OccEmpty) {
					continue;
				}
				occupancy[idx] = OccExterior;
				queue.push_back(idx);
			}
		}

		for (int64_t i = 0; i < voxelCount; ++i) {
			if (occupancy[i] == OccEmpty) {
				occupancy[i] = OccInterior;
			}
		}
	}

	core::DynamicArray<glm::ivec3> surfacePos;
	surfacePos.reserve((int)glm::min(voxelCount, (int64_t)65536));
	for (int z = rmin.z; z <= rmax.z; ++z) {
		for (int y = rmin.y; y <= rmax.y; ++y) {
			for (int x = rmin.x; x <= rmax.x; ++x) {
				if (occupancy[region.index(x, y, z)] == OccSurface) {
					surfacePos.push_back(glm::ivec3(x, y, z));
				}
			}
		}
	}
	Log::info("Solid voxelize: %i surface voxels", (int)surfacePos.size());

	(void)normalPalette;

	const int parallel = app::for_parallel_size(0, surfacePos.size());
	core::DynamicArray<PosMap> localMaps;
	localMaps.resize(core_max(1, parallel));
	core::AtomicInt mapIdx(0);
	auto fn = [&](int start, int end) {
		const int slot = mapIdx.increment();
		PosMap &localMap = localMaps[slot];
		localMap.reserve(end - start);
		for (int i = start; i < end; ++i) {
			if (stopExecution()) {
				return;
			}
			const glm::ivec3 &pos = surfacePos[i];
			const glm::vec3 center((float)pos.x + 0.5f, (float)pos.y + 0.5f, (float)pos.z + 0.5f);
			SurfaceSample best;
			sampleNearestSurface(pos, center, region, tris, head, next, triOf, best);
			if (best.triIdx < 0) {
				continue;
			}
			const voxelformat::MeshTri &tri = tris[best.triIdx];
			const color::RGBA rgba = colorAt(tri, meshMaterialArray, best.uv, false, false, true);
			if (rgba.a <= AlphaThreshold) {
				continue;
			}
			// Cube lighting uses face normals. Stamping the source triangle
			// normal makes a whole voxel shade as a slanted plane.
			addToPosMap(localMap, region, rgba, 1u, NO_NORMAL, pos, tri.materialIdx);
		}
	};
	if (!surfacePos.empty()) {
		app::for_parallel(0, surfacePos.size(), fn);
	}

	core::DynamicArray<glm::ivec3> interiors;
	if (fillHollow) {
		interiors.reserve((int)glm::min(voxelCount / 4, (int64_t)65536));
		for (int z = rmin.z; z <= rmax.z; ++z) {
			for (int y = rmin.y; y <= rmax.y; ++y) {
				for (int x = rmin.x; x <= rmax.x; ++x) {
					if (occupancy[region.index(x, y, z)] == OccInterior) {
						interiors.push_back(glm::ivec3(x, y, z));
					}
				}
			}
		}
	}

	PosMap posMap;
	posMap.reserve((int)surfacePos.size());
	for (const PosMap &localMap : localMaps) {
		for (const auto &entry : localMap) {
			auto iter = posMap.find(entry->key);
			if (iter == posMap.end()) {
				PosSampling copy = entry->second;
				posMap.emplace(entry->key, core::move(copy));
			}
		}
	}

	Log::info("Solid voxelize: %i colored surface voxels, %i interior voxels", (int)posMap.size(),
			  (int)interiors.size());
	voxelizeTris(node, posMap, meshMaterialArray, false, true);

	if (interiors.empty()) {
		return;
	}
	palette::Palette pal = node.palette();
	const color::RGBA white(255, 255, 255, 255);
	uint8_t whiteIdx = 0;
	// tryAdd returns false if the color is already present; index is still set.
	pal.tryAdd(white, false, &whiteIdx, false);
	if (pal.color(whiteIdx) != white) {
		if (pal.colorCount() < palette::PaletteMaxColors) {
			whiteIdx = (uint8_t)pal.colorCount();
			pal.setSize(pal.colorCount() + 1);
			pal.setColor(whiteIdx, white);
		}
	}
	if (pal.color(whiteIdx) != white) {
		Log::error("Solid voxelize: failed to reserve exact white for interiors");
		return;
	}
	node.setPalette(pal);
	voxel::RawVolume *volume = node.volume();
	if (volume == nullptr) {
		return;
	}
	const voxel::Voxel whiteVoxel = voxel::createVoxel(pal, whiteIdx);
	for (const glm::ivec3 &p : interiors) {
		volume->setVoxel(p, whiteVoxel);
	}
}

} // namespace voxelformat
