/**
 * @file
 */

#pragma once

#include <glm/common.hpp>
#include <glm/vec2.hpp>
#include <math.h>
#include <stdint.h>

namespace voxelpathtracer {
namespace sampling {

/**
 * Integer-only hash used to seed every renderer sampling dimension.
 *
 * Keep the operations and constants in sync with the future WGSL version.
 * WGSL u32 arithmetic has the same wrapping behavior.
 */
inline uint32_t hash32(uint32_t value) {
	value = (value ^ 61u) ^ (value >> 16u);
	value *= 9u;
	value ^= value >> 4u;
	value *= 0x27d4eb2du;
	value ^= value >> 15u;
	return value;
}

/**
 * Return one reproducible value in [0, 1) and advance the state.
 */
inline float next1D(uint32_t &state) {
	state = hash32(state);
	return (float)(state >> 8u) * (1.0f / 16777216.0f);
}

/**
 * Cranley-Patterson rotated additive recurrence for progressive sampling.
 * This is intentionally expressed with operations directly available in WGSL.
 */
inline float progressive1D(uint32_t index, uint32_t scramble) {
	const float rotation = (float)(hash32(scramble) >> 8u) * (1.0f / 16777216.0f);
	return glm::fract(rotation + (float)(index + 1u) * 0.7548776662466927f);
}

/**
 * Progressive R2 sequence with independent per-pixel rotations. Unlike two
 * random jitter values, every prefix spreads camera samples over the pixel,
 * improving antialiasing and convergence without increasing the ray budget.
 */
inline glm::vec2 progressive2D(uint32_t index, uint32_t scramble) {
	const float rotationX = (float)(hash32(scramble ^ 0x68bc21ebu) >> 8u) * (1.0f / 16777216.0f);
	const float rotationY = (float)(hash32(scramble ^ 0x02e5be93u) >> 8u) * (1.0f / 16777216.0f);
	const float sample = (float)(index + 1u);
	return glm::vec2(glm::fract(rotationX + sample * 0.7548776662466927f),
					 glm::fract(rotationY + sample * 0.5698402909980532f));
}

/** Henyey-Greenstein phase. Keep in sync with the WGSL twin. */
inline float henyeyGreenstein(float g, float cosTheta) {
	const float gg = glm::clamp(g, -0.95f, 0.95f);
	const float g2 = gg * gg;
	const float den = 1.0f + g2 - 2.0f * gg * cosTheta;
	return (1.0f - g2) / (12.566370614359172f * den * sqrtf(glm::max(den, 1.0e-8f)));
}

} // namespace sampling
} // namespace voxelpathtracer
