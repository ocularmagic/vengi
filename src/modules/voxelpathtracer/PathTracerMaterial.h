/**
 * @file
 */

#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <stddef.h>
#include <stdint.h>

namespace voxelpathtracer {

enum PathTracerSurfaceType : uint32_t {
	PathTracerSurfaceOpaque = 0u,
	PathTracerSurfaceAlpha = 1u,
	PathTracerSurfaceGlass = 2u,
	PathTracerSurfaceMetal = 3u,
	PathTracerSurfaceMedia = 4u
};

inline bool pathTracerSurfaceIsSolid(uint8_t surfaceType) {
	return surfaceType == PathTracerSurfaceOpaque || surfaceType == PathTracerSurfaceMetal;
}

/**
 * GPU-aligned material record shared by CPU traversal and the future WGSL
 * storage buffer. Every field begins on a vec4 boundary and the record stride
 * is a multiple of 16 bytes.
 */
struct alignas(16) PathTracerMaterial {
	// rgb albedo, opacity
	glm::vec4 albedoOpacity{0.0f, 0.0f, 0.0f, 1.0f};
	// rgb surface emission, index of refraction
	glm::vec4 emissionIor{0.0f, 0.0f, 0.0f, 1.0f};
	// rgb volume emission, attenuation
	glm::vec4 volumeEmissionAttenuation{0.0f};
	// metal, roughness, specular, density
	glm::vec4 surface{0.0f, 0.1f, 0.5f, 0.0f};
	// rim light (Henyey-Greenstein asymmetry g), scatter, reserved, reserved
	glm::vec4 volume{0.0f};
	// surface type, reserved, reserved, reserved
	glm::uvec4 flags{0u};

	inline glm::vec3 albedo() const {
		return glm::vec3(albedoOpacity);
	}
	inline float opacity() const {
		return albedoOpacity.w;
	}
	inline glm::vec3 emission() const {
		return glm::vec3(emissionIor);
	}
	inline float ior() const {
		return emissionIor.w;
	}
	inline glm::vec3 volumeEmission() const {
		return glm::vec3(volumeEmissionAttenuation);
	}
	inline float attenuation() const {
		return volumeEmissionAttenuation.w;
	}
	inline float metal() const {
		return surface.x;
	}
	inline float roughness() const {
		return surface.y;
	}
	inline float specular() const {
		return surface.z;
	}
	inline float density() const {
		return surface.w;
	}
	// Henyey-Greenstein asymmetry g. The locked UI name is "Rim light":
	// g > 0 forward scatter (sun-edge glow), g < 0 back scatter.
	inline float rimLight() const {
		return volume.x;
	}
	// How much environment light the volume shows (smoke vs cloud). Locked UI
	// name "Scatter". This is not emission.
	inline float scatter() const {
		return volume.y;
	}
	inline uint8_t surfaceType() const {
		return (uint8_t)flags.x;
	}
};

static_assert(sizeof(PathTracerMaterial) == 96u, "PathTracerMaterial must match the WGSL record stride");
static_assert(alignof(PathTracerMaterial) == 16u, "PathTracerMaterial must remain 16-byte aligned");
static_assert(offsetof(PathTracerMaterial, emissionIor) == 16u, "PathTracerMaterial emission offset changed");
static_assert(offsetof(PathTracerMaterial, volumeEmissionAttenuation) == 32u,
			  "PathTracerMaterial volume emission offset changed");
static_assert(offsetof(PathTracerMaterial, surface) == 48u, "PathTracerMaterial surface offset changed");
static_assert(offsetof(PathTracerMaterial, volume) == 64u, "PathTracerMaterial volume offset changed");
static_assert(offsetof(PathTracerMaterial, flags) == 80u, "PathTracerMaterial flags offset changed");

} // namespace voxelpathtracer
