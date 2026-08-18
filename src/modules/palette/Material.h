/**
 * @file
 */

#pragma once

#include "core/ArrayLength.h"
#include "core/Assert.h"
#include <stdint.h>

namespace palette {

// unused in vengi at the moment, but here to provide a better magicavoxel import/export experience
enum class MaterialType {
	Diffuse = 0, // diffuse is default
	Metal = 1,
	Glass = 2,
	Emit = 3,
	Blend = 4,
	Volumetric = 5,
};

// just a few of these values are used for rendering in vengi - but all are used for import/export if the format
// supports it
enum MaterialProperty : uint32_t {
	MaterialNone = 0,

	MaterialMetal = 1,
	MaterialRoughness = 2,
	MaterialSpecular = 3,
	MaterialIndexOfRefraction = 4,
	MaterialAttenuation = 5,
	MaterialEmitBoost = 6,
	MaterialEmit = 7,
	MaterialLowDynamicRange = 8,
	MaterialDensity = 9,
	MaterialSp = 10,
	MaterialRimLight = 11, // Henyey-Greenstein asymmetry: g > 0 forward scatter, g < 0 back scatter
	MaterialScatter = 12,

	MaterialMax
};

/**
 * @brief Each palette color can have a material assigned to it.
 */
struct Material {
	Material();
	uint32_t mask = MaterialNone;
	MaterialType type = MaterialType::Diffuse;
	// make sure to keep the order of the properties and keep metal
	// as first - see the name string array
	float metal = 0.0f;
	float roughness = 0.0f;
	float specular = 0.0f;
	float indexOfRefraction = 0.0f;
	float attenuation = 0.0f;
	float emitBoost = 0.0f;
	float emit = 0.0f;
	float lowDynamicRange = 0.0f;
	float density = 0.0f;
	float sp = 0.0f;
	float rimLight = 0.0f; // Henyey-Greenstein forward-scatter bias
	float scatter = 0.0f;

	bool operator==(const Material &rhs) const;
	bool operator!=(const Material &rhs) const;
	constexpr inline bool has(MaterialProperty n) const {
		return (mask & (1 << n)) != 0;
	}
	float value(MaterialProperty n) const;
	void setValue(MaterialProperty n, float value);
};

// make sure to keep the order of the properties - see Material struct float values
// none is not included in this array - beware of the -1 offset
static constexpr const char *MaterialPropertyNames[] = {"metal",	   "roughness", "specular", "indexOfRefraction",
														"attenuation", "emitBoost", "emit",	  "lowDynamicRange",
														"density",	   "sp",		  "rimLight", "scatter"};
static_assert(lengthof(MaterialPropertyNames) == MaterialMax - 1, "MaterialPropertyNames size mismatch");

inline const char *MaterialPropertyName(MaterialProperty prop) {
	core_assert(prop > MaterialNone && prop < MaterialMax);
	return MaterialPropertyNames[(int)prop - 1];
}

// make sure to keep the order of the properties - see Material struct float values
struct MaterialMinMax {
	float minVal;
	float maxVal;
};
static constexpr const MaterialMinMax MaterialPropertyMinsMaxs[] = {
	{0.0f, 1.0f}, // metal
	{0.0f, 1.0f}, // roughness
	{0.0f, 1.0f}, // specular
	{0.0f, 3.0f}, // indexOfRefraction
	{0.0f, 1.0f}, // attenuation
	{0.0f, 1.0f}, // emitBoost
	{0.0f, 1.0f}, // emit
	{0.0f, 1.0f}, // lowDynamicRange
	{0.0f, 1.0f}, // density
	{0.0f, 1.0f}, // sp
	{0.0f, 1.0f}, // rimLight
	{0.0f, 1.0f}  // scatter
};
static_assert(lengthof(MaterialPropertyNames) == MaterialMax - 1, "MaterialPropertyNames size mismatch");
inline MaterialMinMax MaterialPropertyMinMax(MaterialProperty prop) {
	core_assert(prop > MaterialNone && prop < MaterialMax);
	return MaterialPropertyMinsMaxs[(int)prop - 1];
}

} // namespace palette
