/**
 * @file
 */

#include "MeshTri.h"
#include <glm/common.hpp>
#include <glm/ext/scalar_common.hpp>
#include <glm/ext/scalar_constants.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/epsilon.hpp>
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/type_aligned.hpp>

namespace voxelformat {

void MeshTri::setUVs(const glm::vec2 &uv1, const glm::vec2 &uv2, const glm::vec2 &uv3) {
	_uv[0] = uv1;
	_uv[1] = uv2;
	_uv[2] = uv3;
}

glm::vec2 MeshTri::centerUV() const {
	return (uv0() + uv1() + uv2()) / 3.0f;
}

int MeshTri::subdivideTriCount(size_t maxPerTriangle) const {
	const float maxSide = maxSideLength();
	if (maxSide <= 1.0f) {
		return 1;
	}
	int d = (int)glm::ceil(glm::log2(maxSide));
	if (d < 0) {
		d = 0;
	}
	if (d > 16) {
		d = 16; // match depth limit in subdivideTri
	}
	size_t trisCount = 1;
	for (int k = 0; k < d; ++k) {
		// multiply by 4 each level, but cap to avoid overflow
		if (trisCount > maxPerTriangle / 4) {
			trisCount = maxPerTriangle;
			break;
		}
		trisCount *= 4;
	}
	return trisCount;
}

float MeshTri::maxSideLength() const {
	const glm::vec3 &v0 = vertex0();
	const glm::vec3 &v1 = vertex1();
	const glm::vec3 &v2 = vertex2();
	const float s0 = glm::length(v0 - v1);
	const float s1 = glm::length(v1 - v2);
	const float s2 = glm::length(v2 - v0);
	const float maxSide = glm::max(glm::max(s0, s1), s2);
	return maxSide;
}

// https://en.wikipedia.org/wiki/Barycentric_coordinate_system
bool MeshTri::calcUVs(const glm::vec3 &pos, glm::vec2 &outUV) const {
	const glm::vec3 &b = calculateBarycentric(pos);

	// Check if barycentric coordinates are within [0, 1]
	if (b.x >= 0.0f && b.x <= 1.0f && b.y >= 0.0f && b.y <= 1.0f && b.z >= 0.0f && b.z <= 1.0f) {
		// Interpolate UVs using barycentric coordinates
		outUV = b.x * uv0() + b.y * uv1() + b.z * uv2();
		return true;
	}

	return false;
}

void MeshTri::closestPoint(const glm::vec3 &pos, glm::vec3 &outPos, glm::vec2 &outUV) const {
	const glm::vec3 &a = vertex0();
	const glm::vec3 &b = vertex1();
	const glm::vec3 &c = vertex2();
	const glm::vec3 ab = b - a;
	const glm::vec3 ac = c - a;
	const glm::vec3 ap = pos - a;

	const float d1 = glm::dot(ab, ap);
	const float d2 = glm::dot(ac, ap);
	if (d1 <= 0.0f && d2 <= 0.0f) {
		outPos = a;
		outUV = uv0();
		return;
	}

	const glm::vec3 bp = pos - b;
	const float d3 = glm::dot(ab, bp);
	const float d4 = glm::dot(ac, bp);
	if (d3 >= 0.0f && d4 <= d3) {
		outPos = b;
		outUV = uv1();
		return;
	}

	const float vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
		const float v = d1 / (d1 - d3);
		outPos = a + v * ab;
		outUV = glm::mix(uv0(), uv1(), v);
		return;
	}

	const glm::vec3 cp = pos - c;
	const float d5 = glm::dot(ab, cp);
	const float d6 = glm::dot(ac, cp);
	if (d6 >= 0.0f && d5 <= d6) {
		outPos = c;
		outUV = uv2();
		return;
	}

	const float vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
		const float w = d2 / (d2 - d6);
		outPos = a + w * ac;
		outUV = glm::mix(uv0(), uv2(), w);
		return;
	}

	const float va = d3 * d6 - d5 * d4;
	if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
		const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		outPos = b + w * (c - b);
		outUV = glm::mix(uv1(), uv2(), w);
		return;
	}

	const float denom = 1.0f / (va + vb + vc);
	const float v = vb * denom;
	const float w = vc * denom;
	const float u = 1.0f - v - w;
	outPos = a + ab * v + ac * w;
	outUV = u * uv0() + v * uv1() + w * uv2();
}

} // namespace voxelformat
