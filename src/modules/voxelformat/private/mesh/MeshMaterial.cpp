/**
 * @file
 */

#include "MeshMaterial.h"
#include "color/ColorUtil.h"
#include <glm/common.hpp>

namespace voxelformat {

MeshMaterialPtr createMaterial(const image::ImagePtr &texture) {
	if (!texture) {
		return {};
	}
	MeshMaterialPtr material = core::make_shared<MeshMaterial>(texture->name());
	material->texture = texture;
	return material;
}

MeshMaterialPtr createMaterial(const core::String &name) {
	return core::make_shared<MeshMaterial>(name);
}

MeshMaterialPtr cloneMaterial(const MeshMaterialPtr &material) {
	return core::make_shared<MeshMaterial>(*material.get());
}

MeshMaterialPtr cloneMaterial(const MeshMaterial &material) {
	return core::make_shared<MeshMaterial>(material);
}

int MeshMaterial::width() const {
	return texture ? texture->width() : 0;
}

int MeshMaterial::height() const {
	return texture ? texture->height() : 0;
}

color::RGBA MeshMaterial::apply(color::RGBA color) const {
	if (baseColorFactor > 0.0f) {
		const float contribution = (1.0f - baseColorFactor);
		color = color::RGBA((float)color.r * contribution + baseColor.r * baseColorFactor,
						   (float)color.g * contribution + baseColor.g * baseColorFactor,
						   (float)color.b * contribution + baseColor.b * baseColorFactor, color.a);
	}
	if (transparency > 0.0f) {
		color.a = color.a * (1.0f - transparency);
	}
	return color;
}

bool MeshMaterial::colorAt(color::RGBA &color, const glm::vec2 &uv, bool originUpperLeft, bool bilinear,
						   bool maxChroma) const {
	if (!texture || !texture->isLoaded()) {
		if (multiplyColorFactor) {
			color = color::linearToSrgb(colorFactor);
			if (transparency > 0.0f) {
				color.a = (uint8_t)((float)color.a * (1.0f - transparency));
			}
			return true;
		}
		if (baseColorFactor <= 0.0f) {
			return false;
		}
		color = color::RGBA(0, 0, 0);
	} else if (maxChroma) {
		color = texture->colorAtMaxChroma(uv, wrapS, wrapT, originUpperLeft);
	} else if (bilinear) {
		color = texture->colorAtBilinear(uv, wrapS, wrapT, originUpperLeft);
	} else {
		color = texture->colorAt(uv, wrapS, wrapT, originUpperLeft);
	}
	if (multiplyColorFactor) {
		const glm::vec4 delta = glm::abs(colorFactor - glm::vec4(1.0f));
		if (delta.r > 0.0001f || delta.g > 0.0001f || delta.b > 0.0001f || delta.a > 0.0001f) {
			const glm::vec4 linear((float)color::srgbToLinear(color.r), (float)color::srgbToLinear(color.g),
								  (float)color::srgbToLinear(color.b), (float)color.a / 255.0f);
			color = color::linearToSrgb(linear * colorFactor);
		}
	} else {
		color = apply(color);
	}
	return true;
}

} // namespace voxelformat
