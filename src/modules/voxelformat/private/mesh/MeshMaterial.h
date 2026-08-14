/**
 * @file
 */

#pragma once

#include "core/SharedPtr.h"
#include "core/collection/DynamicArray.h"
#include "core/collection/StringMap.h"
#include "image/Image.h"
#include "palette/Material.h"
#include <glm/vec4.hpp>

namespace voxelformat {

class MeshMaterial {
public:
	MeshMaterial(const core::String &_name) : name(_name) {
	}
	core::String name;
	image::ImagePtr texture;
	image::TextureWrap wrapS = image::TextureWrap::Repeat;
	image::TextureWrap wrapT = image::TextureWrap::Repeat;
	palette::Material material;
	color::RGBA baseColor{255, 255, 255, 255};
	color::RGBA emitColor{0, 0, 0, 0};
	float baseColorFactor = 0.0f;
	/**
	 * Linear RGB(A) multiplier applied to the sampled albedo (glTF baseColorFactor).
	 * When @c multiplyColorFactor is true, texture samples are decoded from sRGB,
	 * multiplied, then encoded back to sRGB before they enter the palette.
	 */
	glm::vec4 colorFactor{1.0f, 1.0f, 1.0f, 1.0f};
	bool multiplyColorFactor = false;
	/**
	 * glTF 2.0 UV origin is the upper-left of the image. Most other mesh
	 * paths use OpenGL-style lower-left (the default).
	 */
	bool uvOriginUpperLeft = false;
	float transparency = 0.0f;
	int16_t uvIndex = 0; // the index of the texture coordinate set used by this material

	int width() const;
	int height() const;
	[[nodiscard]] color::RGBA apply(color::RGBA color) const;
	[[nodiscard]] bool colorAt(color::RGBA &color, const glm::vec2 &uv, bool originUpperLeft = false,
							   bool bilinear = false, bool maxChroma = false) const;
};

using MeshMaterialPtr = core::SharedPtr<MeshMaterial>;

MeshMaterialPtr createMaterial(const image::ImagePtr &texture);
MeshMaterialPtr createMaterial(const core::String &name);
MeshMaterialPtr cloneMaterial(const MeshMaterialPtr &material);
MeshMaterialPtr cloneMaterial(const MeshMaterial &material);

using MeshMaterialIndex = int16_t;
using MeshMaterialMap = core::StringMap<MeshMaterialIndex>;
using MeshMaterialArray = core::DynamicArray<MeshMaterialPtr>;

} // namespace voxelformat
