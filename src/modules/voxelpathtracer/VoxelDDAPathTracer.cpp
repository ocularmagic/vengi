/**
 * @file
 */

#include "VoxelDDAPathTracer.h"
#include "Appearance.h"
#include "PathTracerHdri.h"
#include "PathTracerSampling.h"
#include "PathTracerTonemap.h"
#include "PathTracerTraversal.h"
#include "app/ForParallel.h"
#include "color/ColorUtil.h"
#include "core/Common.h"
#include "core/Log.h"
#include "core/StandardLib.h"
#include "image/Image.h"
#include "io/File.h"
#include "math/Ray.h"
#include "palette/Material.h"
#include "palette/Palette.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "voxel/RawVolume.h"
#include "voxel/Voxel.h"
#include "core/GLM.h"

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/norm.hpp>

#include <stb_image.h>
#include <yocto_color.h>

namespace voxelpathtracer {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kInvPi = 1.0f / kPi;
constexpr float kEps = 1.0e-4f;
constexpr float kSpawnEps = 1.0e-3f;
constexpr uint8_t kSurfOpaque = (uint8_t)PathTracerSurfaceOpaque;
constexpr uint8_t kSurfAlpha = (uint8_t)PathTracerSurfaceAlpha;
constexpr uint8_t kSurfGlass = (uint8_t)PathTracerSurfaceGlass;
constexpr uint8_t kSurfMetal = (uint8_t)PathTracerSurfaceMetal;
constexpr uint8_t kSurfMedia = (uint8_t)PathTracerSurfaceMedia;

// Adaptive sampling constants. Keep the minimum-sample floor and the absolute
// luminance noise floor in sync with the WGSL convergence predicate.
constexpr int kAdaptiveMinSamples = 16;
constexpr float kAdaptiveNoiseFloor = 0.01f;

#ifdef __EMSCRIPTEN__
static PathTracerLightingData webGPULightingData(const PathTracerState &state, bool hasHdri) {
	PathTracerLightingData lighting;
	lighting.environmentColor = glm::vec4(state.environmentColor.x, state.environmentColor.y,
		state.environmentColor.z, 0.0f);
	const glm::vec3 sunDirection(glm::cos(state.sunAzimuth) * glm::cos(state.sunElevation),
		glm::sin(state.sunElevation), glm::sin(state.sunAzimuth) * glm::cos(state.sunElevation));
	lighting.sunDirectionIntensity = glm::vec4(glm::normalize(sunDirection), state.sunIntensity);
	lighting.environmentParams = glm::vec4(state.hdriIntensity, state.hdriAzimuth,
		glm::max(1.0f, state.params.clamp), 0.0f);
	lighting.flags.x = state.hdriEnvironment && hasHdri ? 2u : (state.skyEnvironment ? 1u : 0u);
	lighting.flags.y = state.params.envhidden ? 1u : 0u;
	lighting.flags.z = state.studioEdges ? 1u : 0u;
	lighting.flags.w = static_cast<uint32_t>(glm::clamp(state.params.bounces, 1, 8));
	return lighting;
}
#endif

static bool isSolidSurf(uint8_t surf) {
	return pathTracerSurfaceIsSolid(surf);
}

static float volumeSigmaT(float density) {
	const float d = glm::clamp(density, 0.0f, 1.0f);
	// 0 is haze through a few voxels. Mid-slider is still see-through.
	// 1 is thick smoke. A linear 0.25+7.75*d was a brick by 0.5.
	return d * d * 2.0f + d * 0.2f;
}

static float henyeyGreenstein(float g, float cosTheta) {
	return sampling::henyeyGreenstein(g, cosTheta);
}

static float fresnelSchlick(float cosi, float ior) {
	float r0 = (1.0f - ior) / (1.0f + ior);
	r0 *= r0;
	const float x = 1.0f - cosi;
	return r0 + (1.0f - r0) * x * x * x * x * x;
}

uint32_t wangHash(uint32_t x) {
	return sampling::hash32(x);
}

static float powerHeuristic(float pdfA, float pdfB) {
	const float a2 = pdfA * pdfA;
	const float b2 = pdfB * pdfB;
	return a2 / glm::max(a2 + b2, 1.0e-12f);
}

static glm::vec3 sampleUniformSphere(const glm::vec2 &sequence) {
	const float z = 1.0f - 2.0f * sequence.x;
	const float r = glm::sqrt(glm::max(0.0f, 1.0f - z * z));
	const float phi = 2.0f * kPi * sequence.y;
	return glm::vec3(r * glm::cos(phi), z, r * glm::sin(phi));
}

constexpr float kMediaStep = 0.28f;

static void tangentBasis(const glm::vec3 &n, glm::vec3 &t, glm::vec3 &b) {
	const glm::vec3 a = (glm::abs(n.x) > 0.1f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
	t = glm::normalize(glm::cross(a, n));
	b = glm::cross(n, t);
}

glm::vec3 cosineHemisphere(const glm::vec3 &n, const glm::vec2 &sequence) {
	const float r = glm::sqrt(sequence.x);
	const float phi = 2.0f * kPi * sequence.y;
	glm::vec3 t, b;
	tangentBasis(n, t, b);
	return glm::normalize(t * (r * glm::cos(phi)) + b * (r * glm::sin(phi)) + n * glm::sqrt(1.0f - sequence.x));
}

static float ggxD(float ndh, float a) {
	const float a2 = a * a;
	const float d = ndh * ndh * (a2 - 1.0f) + 1.0f;
	return a2 / (kPi * d * d);
}

static float ggxG1(float ndx, float a) {
	const float nd = glm::max(ndx, 0.0f);
	const float a2 = a * a;
	return 2.0f * nd / (nd + glm::sqrt(a2 + (1.0f - a2) * nd * nd));
}

static glm::vec3 fresnelSchlick3(const glm::vec3 &f0, float vdh) {
	const float t = 1.0f - glm::clamp(vdh, 0.0f, 1.0f);
	const float t2 = t * t;
	const float t5 = t2 * t2 * t;
	return f0 + (glm::vec3(1.0f) - f0) * t5;
}

static glm::vec4 paletteColorLinear(const color::RGBA &rgba) {
	return glm::vec4((float)color::srgbToLinear(rgba.r), (float)color::srgbToLinear(rgba.g),
					 (float)color::srgbToLinear(rgba.b), (float)rgba.a / 255.0f);
}

static glm::vec3 sampleGgxVisibleHalf(const glm::vec3 &n, const glm::vec3 &wo, float a,
									  const glm::vec2 &sequence) {
	// Heitz isotropic GGX visible-normal sampling. Sampling the NDF alone
	// wastes many grazing samples below the surface and produces bright,
	// sparse highlights at practical sample counts.
	glm::vec3 t, b;
	tangentBasis(n, t, b);
	const glm::vec3 view(glm::dot(wo, t), glm::dot(wo, b), glm::max(glm::dot(wo, n), 1.0e-6f));
	const glm::vec3 vh = glm::normalize(glm::vec3(a * view.x, a * view.y, view.z));
	const float lensq = vh.x * vh.x + vh.y * vh.y;
	const glm::vec3 t1 = lensq > 1.0e-8f ? glm::vec3(-vh.y, vh.x, 0.0f) / glm::sqrt(lensq) :
		glm::vec3(1.0f, 0.0f, 0.0f);
	const glm::vec3 t2 = glm::cross(vh, t1);
	const float r = glm::sqrt(sequence.x);
	const float phi = 2.0f * kPi * sequence.y;
	const float diskX = r * glm::cos(phi);
	float diskY = r * glm::sin(phi);
	const float s = 0.5f * (1.0f + vh.z);
	diskY = (1.0f - s) * glm::sqrt(glm::max(0.0f, 1.0f - diskX * diskX)) + s * diskY;
	const float diskZ = glm::sqrt(glm::max(0.0f, 1.0f - diskX * diskX - diskY * diskY));
	const glm::vec3 nh = diskX * t1 + diskY * t2 + diskZ * vh;
	const glm::vec3 ne = glm::normalize(glm::vec3(a * nh.x, a * nh.y, glm::max(nh.z, 0.0f)));
	return glm::normalize(t * ne.x + b * ne.y + n * ne.z);
}

// f * NdotL for a metal-roughness mix. metal=0 is Lambert.
static glm::vec3 evalOpaqueFcos(const glm::vec3 &albedo, float metal, float spec, float a, const glm::vec3 &n,
								const glm::vec3 &wo, const glm::vec3 &wi) {
	const float ndl = glm::dot(n, wi);
	const float ndv = glm::dot(n, wo);
	if (ndl <= 1.0e-6f || ndv <= 1.0e-6f) {
		return glm::vec3(0.0f);
	}
	const glm::vec3 h = glm::normalize(wo + wi);
	const float ndh = glm::max(glm::dot(n, h), 0.0f);
	const float vdh = glm::max(glm::dot(wo, h), 0.0f);
	const float D = ggxD(ndh, a);
	const float G = ggxG1(ndv, a) * ggxG1(ndl, a);
	const float specAmt = 0.2f + 0.8f * glm::clamp(spec, 0.0f, 1.0f);
	const glm::vec3 f0 = glm::mix(glm::vec3(0.04f * specAmt), albedo, glm::clamp(metal, 0.0f, 1.0f));
	const glm::vec3 F = fresnelSchlick3(f0, vdh);
	const glm::vec3 diffuse = albedo * (glm::vec3(1.0f) - F) * (1.0f - metal) * (kInvPi * ndl);
	return diffuse + F * (D * G / (4.0f * ndv));
}

static float opaquePdf(float metal, float spec, const glm::vec3 &albedo, float a, const glm::vec3 &n,
						 const glm::vec3 &wo, const glm::vec3 &wi) {
	const float ndl = glm::max(glm::dot(n, wi), 0.0f);
	const float ndv = glm::max(glm::dot(n, wo), 0.0f);
	if (ndl <= 1.0e-6f || ndv <= 1.0e-6f) {
		return 0.0f;
	}
	const glm::vec3 f0 = glm::mix(glm::vec3(0.04f * (0.2f + 0.8f * spec)), albedo, metal);
	const glm::vec3 Fv = fresnelSchlick3(f0, ndv);
	const float pSpec = glm::clamp(0.25f + 0.75f * (Fv.x + Fv.y + Fv.z) * (1.0f / 3.0f), 0.15f, 0.95f);
	const glm::vec3 hsum = wo + wi;
	float specPdf = 0.0f;
	if (glm::length2(hsum) > 1.0e-12f) {
		const glm::vec3 h = glm::normalize(hsum);
		const float ndh = glm::max(glm::dot(n, h), 0.0f);
		// Visible-normal GGX reflection PDF. The VdotH terms from the
		// visible-normal density and reflection Jacobian cancel.
		specPdf = ggxD(ndh, a) * ggxG1(ndv, a) / glm::max(4.0f * ndv, 1.0e-6f);
	}
	return pSpec * specPdf + (1.0f - pSpec) * ndl * kInvPi;
}

float studioBevel(const glm::vec3 &pos, const glm::vec3 &normal) {
	const glm::vec3 f = glm::fract(pos);
	float edge;
	if (glm::abs(normal.y) > 0.5f) {
		edge = glm::min(glm::min(f.x, 1.0f - f.x), glm::min(f.z, 1.0f - f.z));
	} else if (glm::abs(normal.x) > 0.5f) {
		edge = glm::min(glm::min(f.y, 1.0f - f.y), glm::min(f.z, 1.0f - f.z));
	} else {
		edge = glm::min(glm::min(f.x, 1.0f - f.x), glm::min(f.y, 1.0f - f.y));
	}
	const float t = glm::clamp((edge - 0.008f) / (0.028f - 0.008f), 0.0f, 1.0f);
	const float rim = 1.0f - t;
	// Keep the seam readable after subpixel integration and denoising. This is
	// a material modulation on the hit voxel, not a screen-space line, so it
	// remains stable under camera movement and transfers directly to WGSL.
	return glm::mix(1.02f, 0.62f, rim);
}

glm::vec3 sampleLatLong(const core::Buffer<float> &rgba, int w, int h, const glm::vec3 &dir) {
	if (w <= 0 || h <= 0 || rgba.empty()) {
		return glm::vec3(0.91f);
	}
	float u = glm::atan(dir.z, dir.x) / (2.0f * kPi);
	if (u < 0.0f) {
		u += 1.0f;
	}
	const float v = glm::acos(glm::clamp(dir.y, -1.0f, 1.0f)) / kPi;
	const float fx = u * (float)w - 0.5f;
	const float fy = v * (float)h - 0.5f;
	const int x0 = (int)glm::floor(fx);
	const int y0 = (int)glm::floor(fy);
	const float tx = fx - (float)x0;
	const float ty = fy - (float)y0;
	auto fetch = [&](int x, int y) {
		int wx = x % w;
		if (wx < 0) {
			wx += w;
		}
		const int wy = glm::clamp(y, 0, h - 1);
		const int i = (wy * w + wx) * 4;
		return glm::vec3(rgba[i], rgba[i + 1], rgba[i + 2]);
	};
	const glm::vec3 c00 = fetch(x0, y0);
	const glm::vec3 c10 = fetch(x0 + 1, y0);
	const glm::vec3 c01 = fetch(x0, y0 + 1);
	const glm::vec3 c11 = fetch(x0 + 1, y0 + 1);
	return glm::mix(glm::mix(c00, c10, tx), glm::mix(c01, c11, tx), ty);
}

} // namespace

VoxelDDAPathTracer::VoxelDDAPathTracer() = default;

VoxelDDAPathTracer::~VoxelDDAPathTracer() {
	stop();
#ifdef __EMSCRIPTEN__
	_webGPU.shutdown();
#endif
}

PathTracerState &VoxelDDAPathTracer::state() {
	return _state;
}

const PathTracerState &VoxelDDAPathTracer::state() const {
	return _state;
}

void VoxelDDAPathTracer::applyAppearanceFromScene(const scenegraph::SceneGraph &sceneGraph) {
	voxelpathtracer::applyAppearanceFromScene(_state, sceneGraph);
}

bool VoxelDDAPathTracer::writeAppearanceToScene(const scenegraph::SceneGraph &sceneGraph) const {
	return voxelpathtracer::writeAppearanceToScene(_state, sceneGraph);
}

bool VoxelDDAPathTracer::loadHdri(const core::String &path) {
	_envRgba.release();
	_envCdf.release();
	_envCdfSum = 0.0f;
	_envW = 0;
	_envH = 0;
	_envIsHdri = false;
	if (!pathTracerLoadHdriFloats(path, _envRgba, _envW, _envH)) {
		return false;
	}
	_envIsHdri = true;
	buildEnvCdf();
	return true;
}

void VoxelDDAPathTracer::buildEnvCdf() {
	_envCdf.release();
	_envCdfSum = 0.0f;
	if (!_envIsHdri || _envW <= 0 || _envH <= 0 || _envRgba.empty()) {
		return;
	}
	const int n = _envW * _envH;
	_envCdf.resize((size_t)n);
	for (int y = 0; y < _envH; ++y) {
		const float sinTheta = glm::sin(kPi * ((float)y + 0.5f) / (float)_envH);
		for (int x = 0; x < _envW; ++x) {
			const int i = y * _envW + x;
			const int p = i * 4;
			const float lum = 0.2126f * _envRgba[p] + 0.7152f * _envRgba[p + 1] + 0.0722f * _envRgba[p + 2];
			_envCdfSum += glm::max(lum, 0.0f) * sinTheta;
			_envCdf[i] = _envCdfSum;
		}
	}
}

void VoxelDDAPathTracer::buildGrids(const scenegraph::SceneGraph &sceneGraph) {
	_scene.clear();
	_hasMedia = false;
	_scene.ground.boundsMin = glm::vec4(1.0e30f, 1.0e30f, 1.0e30f, 0.0f);
	_scene.ground.boundsMax = glm::vec4(-1.0e30f, 0.0f, -1.0e30f, 0.0f);

	for (const auto &e : sceneGraph.nodes()) {
		const scenegraph::SceneGraphNode &node = e->value;
		if (!node.isAnyModelNode() || !node.visible()) {
			continue;
		}
		const voxel::RawVolume *volume = sceneGraph.resolveVolume(node);
		if (volume == nullptr) {
			continue;
		}
		const voxel::Region &region = sceneGraph.resolveRegion(node);
		if (!region.isValid()) {
			continue;
		}
		const palette::Palette &palette = sceneGraph.resolvePalette(node);
		scenegraph::KeyFrameIndex keyFrameIdx = 0;
		const scenegraph::SceneGraphTransform &transform = node.transform(keyFrameIdx);
		const glm::vec3 size(region.getDimensionsInVoxels());
		const glm::vec3 objPivot = node.pivot() * size;

		const glm::mat4 worldMat = transform.worldMatrix();
		PathTracerGrid &grid = _scene.addGrid(region.getLowerCorner(), region.getDimensionsInVoxels(), worldMat,
											 glm::inverse(worldMat), objPivot);
		const glm::ivec3 gridMins = grid.mins();
		const glm::ivec3 gridSize = grid.size();

		for (int i = 0; i < 256; ++i) {
			const glm::vec4 c = paletteColorLinear(palette.color((uint8_t)i));
			const palette::Material &mat = palette.material((uint8_t)i);
			const float emit = mat.has(palette::MaterialProperty::MaterialEmit) ? mat.value(palette::MaterialProperty::MaterialEmit) : 0.0f;
			glm::vec3 surfaceEmission(0.0f);
			glm::vec3 volumeEmission(0.0f);
			if (emit > 0.0f) {
				// emit is 0..1 in the UI. Solids use a steep curve so the
				// same slider covers a night-light through a strong lamp.
				// Volumes use a gentler curve: modest emit is a soft flame,
				// not a clipped neon cube.
				float emitBoostMul = 1.0f;
				if (mat.has(palette::MaterialProperty::MaterialEmitBoost)) {
					emitBoostMul = glm::exp2(mat.value(palette::MaterialProperty::MaterialEmitBoost) * 5.0f);
				}
				surfaceEmission = glm::vec3(c) * (glm::exp2(emit * 9.0f) - 1.0f) * emitBoostMul;
				volumeEmission = glm::vec3(c) * (glm::exp2(emit * 3.0f) - 1.0f) * emitBoostMul;
			}
			const float opacity = glm::clamp(c.a, 0.0f, 1.0f);
			const float attenuation = mat.has(palette::MaterialProperty::MaterialAttenuation)
								  ? mat.value(palette::MaterialProperty::MaterialAttenuation)
								  : 0.0f;
			float ior = 1.0f;
			if (mat.has(palette::MaterialProperty::MaterialIndexOfRefraction)) {
				ior = glm::max(mat.value(palette::MaterialProperty::MaterialIndexOfRefraction), 1.0f);
			}
			float metal = mat.has(palette::MaterialProperty::MaterialMetal)
							  ? mat.value(palette::MaterialProperty::MaterialMetal)
							  : 0.0f;
			// Type Metal with the slider at 0 is still a metal (Magica).
			if (mat.type == palette::MaterialType::Metal && metal <= 0.0f) {
				metal = 1.0f;
			}
			metal = glm::clamp(metal, 0.0f, 1.0f);
			// Default roughness is 0.1 on every slot -- that is not "this is metal".
			const float roughness = mat.has(palette::MaterialProperty::MaterialRoughness)
									? glm::clamp(mat.value(palette::MaterialProperty::MaterialRoughness), 0.04f, 1.0f)
									: 0.1f;
			const float specular = mat.has(palette::MaterialProperty::MaterialSpecular)
								   ? glm::clamp(mat.value(palette::MaterialProperty::MaterialSpecular), 0.0f, 1.0f)
								   : 0.5f;
			float density = mat.has(palette::MaterialProperty::MaterialDensity)
								? mat.value(palette::MaterialProperty::MaterialDensity)
								: 0.0f;
			if (mat.type == palette::MaterialType::Volumetric && density <= 0.0f) {
				density = 0.45f;
			}
			density = glm::clamp(density, 0.0f, 1.0f);
			const float rimLight = mat.has(palette::MaterialProperty::MaterialRimLight)
								? glm::clamp(mat.value(palette::MaterialProperty::MaterialRimLight), 0.0f, 1.0f)
								: 0.0f;
			const float scatter = mat.has(palette::MaterialProperty::MaterialScatter)
								? glm::clamp(mat.value(palette::MaterialProperty::MaterialScatter), 0.0f, 1.0f)
								: (mat.type == palette::MaterialType::Volumetric ? 0.85f : 0.0f);
			// Default IOR is 1.3 on every slot -- that is not "this is glass".
			// See-through only if the swatch has alpha or Magica marked Glass/Blend.
			uint32_t surfaceType = kSurfOpaque;
			if (mat.type == palette::MaterialType::Volumetric || density > 1.0e-3f) {
				surfaceType = kSurfMedia;
			} else if (mat.type == palette::MaterialType::Glass || (opacity < 1.0f && ior > 1.01f)) {
				surfaceType = kSurfGlass;
			} else if (mat.type == palette::MaterialType::Blend || opacity < 1.0f) {
				surfaceType = kSurfAlpha;
			} else if (metal > 1.0e-3f) {
				surfaceType = kSurfMetal;
			}
			PathTracerMaterial &material = _scene.material(grid, (uint8_t)i);
			material.albedoOpacity = glm::vec4(glm::vec3(c), opacity);
			material.emissionIor = glm::vec4(surfaceEmission, ior);
			material.volumeEmissionAttenuation = glm::vec4(volumeEmission, attenuation);
			material.surface = glm::vec4(metal, roughness, specular, density);
			material.volume = glm::vec4(rimLight, scatter, 0.0f, 0.0f);
			material.flags = glm::uvec4(surfaceType, 0u, 0u, 0u);
			if (surfaceType == kSurfMedia) {
				_hasMedia = true;
			}
		}

		bool any = false;
		for (int z = gridMins.z; z < gridMins.z + gridSize.z; ++z) {
			for (int y = gridMins.y; y < gridMins.y + gridSize.y; ++y) {
				for (int x = gridMins.x; x < gridMins.x + gridSize.x; ++x) {
					const voxel::Voxel &voxel = volume->voxel(x, y, z);
					if (voxel::isAir(voxel.getMaterial())) {
						continue;
					}
					const int ix = x - gridMins.x;
					const int iy = y - gridMins.y;
					const int iz = z - gridMins.z;
					const uint32_t idx = ((uint32_t)iz * (uint32_t)gridSize.y + (uint32_t)iy) *
										 (uint32_t)gridSize.x + (uint32_t)ix;
					_scene.cell(grid, idx) = (uint32_t)voxel.getColor() + 1u;
					any = true;
					for (int iz = 0; iz <= 1; ++iz) {
						for (int iy = 0; iy <= 1; ++iy) {
							for (int ix = 0; ix <= 1; ++ix) {
								const glm::vec3 local((float)(x + ix), (float)(y + iy), (float)(z + iz));
								const glm::vec3 world = glm::vec3(grid.worldMat *
																 (glm::vec4(local, 1.0f) - glm::vec4(grid.pivot(), 0.0f)));
								_scene.ground.boundsMin.x = glm::min(_scene.ground.boundsMin.x, world.x);
								_scene.ground.boundsMax.x = glm::max(_scene.ground.boundsMax.x, world.x);
								_scene.ground.boundsMin.z = glm::min(_scene.ground.boundsMin.z, world.z);
								_scene.ground.boundsMax.z = glm::max(_scene.ground.boundsMax.z, world.z);
								if (iy == 0) {
									_scene.ground.boundsMin.y = glm::min(_scene.ground.boundsMin.y, world.y);
								}
							}
						}
					}
				}
			}
		}
		if (any) {
			_scene.ground.boundsMin.w = 1.0f;
		} else {
			_scene.cells.resize(grid.cellOffset());
			_scene.materials.resize(grid.materialOffset());
			_scene.grids.pop();
		}
	}
	if (_scene.ground.enabled()) {
		const float spanX = _scene.ground.boundsMax.x - _scene.ground.boundsMin.x;
		const float spanZ = _scene.ground.boundsMax.z - _scene.ground.boundsMin.z;
		const float pad = glm::max(16.0f, glm::max(spanX, spanZ));
		_scene.ground.boundsMin.x -= pad;
		_scene.ground.boundsMax.x += pad;
		_scene.ground.boundsMin.z -= pad;
		_scene.ground.boundsMax.z += pad;
	}
	if (!_state.groundPlane) {
		_scene.ground.boundsMin.w = 0.0f;
	}
	buildEmitLights();
}

glm::vec3 VoxelDDAPathTracer::toWorld(const PathTracerGrid &grid, const glm::vec3 &local) const {
	return glm::vec3(grid.worldMat * (glm::vec4(local, 1.0f) - glm::vec4(grid.pivot(), 0.0f)));
}

static void sampleVoxelFace(const glm::ivec3 &cell, int face, float u, float v, glm::vec3 &localP,
							glm::vec3 &localN);

void VoxelDDAPathTracer::buildEmitLights() {
	_scene.emitters.clear();
	static const glm::ivec3 faceDir[6] = {glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 1, 0),
											 glm::ivec3(0, -1, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1)};
	for (int gi = 0; gi < (int)_scene.grids.size(); ++gi) {
		const PathTracerGrid &grid = _scene.grids[gi];
		const glm::ivec3 gridMins = grid.mins();
		const glm::ivec3 gridSize = grid.size();
		const int n = gridSize.x * gridSize.y * gridSize.z;
		for (int i = 0; i < n; ++i) {
			const uint32_t packed = _scene.cell(grid, (uint32_t)i);
			if (packed == 0) {
				continue;
			}
			const uint8_t color = (uint8_t)(packed - 1);
			// Volume glow is integrated in marchVolume. Putting media cells
			// on the area-light list floods the floor like a neon cube.
			if (_scene.material(grid, color).surfaceType() == kSurfMedia) {
				continue;
			}
			const glm::vec3 le = _scene.material(grid, color).emission();
			if (glm::max(le.x, glm::max(le.y, le.z)) <= 0.0f) {
				continue;
			}
			const int ix = i % gridSize.x;
			const int iy = (i / gridSize.x) % gridSize.y;
			const int iz = i / (gridSize.x * gridSize.y);
			const glm::ivec3 cell = gridMins + glm::ivec3(ix, iy, iz);
			for (int face = 0; face < 6; ++face) {
				const glm::ivec3 neighbor = cell + faceDir[face];
				bool blocked = false;
				if (neighbor.x >= gridMins.x && neighbor.y >= gridMins.y && neighbor.z >= gridMins.z &&
					neighbor.x < gridMins.x + gridSize.x && neighbor.y < gridMins.y + gridSize.y &&
					neighbor.z < gridMins.z + gridSize.z) {
					const uint32_t np = _scene.cell(grid, grid.localCellIndex(neighbor));
					blocked = np != 0 && isSolidSurf(_scene.material(grid, (uint8_t)(np - 1u)).surfaceType());
				}
				if (blocked) {
					continue;
				}
				glm::vec3 localOrigin;
				glm::vec3 localU;
				glm::vec3 localV;
				glm::vec3 localNormal;
				sampleVoxelFace(cell, face, 0.0f, 0.0f, localOrigin, localNormal);
				sampleVoxelFace(cell, face, 1.0f, 0.0f, localU, localNormal);
				sampleVoxelFace(cell, face, 0.0f, 1.0f, localV, localNormal);
				const glm::vec3 worldOrigin = toWorld(grid, localOrigin);
				const glm::vec3 worldU = toWorld(grid, localU) - worldOrigin;
				const glm::vec3 worldV = toWorld(grid, localV) - worldOrigin;
				const float area = glm::length(glm::cross(worldU, worldV));
				glm::vec3 worldNormal = glm::transpose(glm::mat3(grid.invWorldMat)) * localNormal;
				const float normalLength = glm::length(worldNormal);
				if (area <= 1.0e-8f || normalLength <= 1.0e-8f) {
					continue;
				}
				worldNormal /= normalLength;
				PathTracerEmitter emitter;
				emitter.originArea = glm::vec4(worldOrigin, area);
				emitter.edgeU = glm::vec4(worldU, 0.0f);
				emitter.edgeV = glm::vec4(worldV, 0.0f);
				emitter.normalData = glm::vec4(worldNormal, 0.0f);
				emitter.emissionData = glm::vec4(le, 0.0f);
				emitter.cellGrid = glm::ivec4(cell, gi);
				_scene.emitters.push_back(emitter);
			}
		}
	}
}

static void sampleVoxelFace(const glm::ivec3 &cell, int face, float u, float v, glm::vec3 &localP, glm::vec3 &localN) {
	const float x0 = (float)cell.x;
	const float y0 = (float)cell.y;
	const float z0 = (float)cell.z;
	switch (face) {
	case 0:
		localP = glm::vec3(x0 + 1.0f, y0 + u, z0 + v);
		localN = glm::vec3(1.0f, 0.0f, 0.0f);
		break;
	case 1:
		localP = glm::vec3(x0, y0 + u, z0 + v);
		localN = glm::vec3(-1.0f, 0.0f, 0.0f);
		break;
	case 2:
		localP = glm::vec3(x0 + u, y0 + 1.0f, z0 + v);
		localN = glm::vec3(0.0f, 1.0f, 0.0f);
		break;
	case 3:
		localP = glm::vec3(x0 + u, y0, z0 + v);
		localN = glm::vec3(0.0f, -1.0f, 0.0f);
		break;
	case 4:
		localP = glm::vec3(x0 + u, y0 + v, z0 + 1.0f);
		localN = glm::vec3(0.0f, 0.0f, 1.0f);
		break;
	default:
		localP = glm::vec3(x0 + u, y0 + v, z0);
		localN = glm::vec3(0.0f, 0.0f, -1.0f);
		break;
	}
}

bool VoxelDDAPathTracer::visibleToLight(const glm::vec3 &orig, const glm::vec3 &target, bool fromGround, int skipGrid,
										const glm::ivec3 &skipCell, int lightGrid, const glm::ivec3 &lightCell) const {
	glm::vec3 d = target - orig;
	const float dist = glm::length(d);
	if (dist <= kSpawnEps) {
		return false;
	}
	d /= dist;
	const Hit h = trace(orig, d, skipGrid, skipCell, true);
	if (!h.hit) {
		return true;
	}
	if (h.ground && h.t + kSpawnEps < dist) {
		return false;
	}
	if (h.gridIndex == lightGrid && h.cell == lightCell) {
		return true;
	}
	return h.t + 0.02f >= dist;
}

bool VoxelDDAPathTracer::sampleEmitLight(const glm::vec3 &p, const glm::vec3 &n, uint32_t sequenceIndex,
										 uint32_t scramble, int skipGrid, const glm::ivec3 &skipCell,
										 glm::vec3 &dir, glm::vec3 &radiance, float &pdf) const {
	radiance = glm::vec3(0.0f);
	pdf = 0.0f;
	const int nLights = (int)_scene.emitters.size();
	if (nLights <= 0) {
		return false;
	}
	const float selection = sampling::sobol1D(sequenceIndex, scramble ^ 0x27d4eb2du);
	int li = (int)(selection * (float)nLights);
	if (li >= nLights) {
		li = nLights - 1;
	}
	const PathTracerEmitter &light = _scene.emitters[li];
	const glm::vec2 faceSample = sampling::sobol2D(sequenceIndex, scramble ^ 0xb5297a4du);
	const glm::vec3 lightP = light.origin() + glm::vec3(light.edgeU) * faceSample.x +
							 glm::vec3(light.edgeV) * faceSample.y;
	const glm::vec3 worldN = light.normal();
	glm::vec3 toL = lightP - p;
	const float dist2 = glm::dot(toL, toL);
	if (dist2 < 1.0e-8f) {
		return false;
	}
	const float dist = glm::sqrt(dist2);
	dir = toL / dist;
	const float ndl = glm::dot(n, dir);
	const float nL = glm::dot(worldN, -dir);
	const bool volumePoint = glm::length2(n) < 1.0e-4f;
	if ((!volumePoint && ndl <= 1.0e-6f) || nL <= 1.0e-6f) {
		return false;
	}
	const glm::vec3 Tr = shadowTransmittance(p, dir, dist, skipGrid, skipCell, light.gridIndex(), light.cell());
	if (glm::max(Tr.x, glm::max(Tr.y, Tr.z)) < 1.0e-5f) {
		return false;
	}
	const float pArea = 1.0f / ((float)nLights * light.area());
	pdf = pArea * dist2 / nL;
	radiance = light.emission() * Tr;
	return pdf > 0.0f;
}

float VoxelDDAPathTracer::emitLightPdf(const glm::vec3 &p, const Hit &hit) const {
	const int nLights = (int)_scene.emitters.size();
	if (nLights <= 0 || hit.ground || hit.gridIndex < 0 ||
		glm::max(hit.emit.x, glm::max(hit.emit.y, hit.emit.z)) <= 0.0f) {
		return 0.0f;
	}
	const glm::vec3 toLight = hit.pos - p;
	const float dist2 = glm::dot(toLight, toLight);
	if (dist2 <= 1.0e-8f) {
		return 0.0f;
	}
	const glm::vec3 dir = toLight / glm::sqrt(dist2);
	for (const PathTracerEmitter &light : _scene.emitters) {
		if (light.gridIndex() != hit.gridIndex || light.cell() != hit.cell || light.area() <= 1.0e-8f) {
			continue;
		}
		const glm::vec3 worldN = light.normal();
		if (glm::dot(worldN, hit.normal) < 0.999f) {
			continue;
		}
		const float cosLight = glm::dot(worldN, -dir);
		if (cosLight <= 1.0e-6f) {
			return 0.0f;
		}
		return dist2 / ((float)nLights * light.area() * cosLight);
	}
	return 0.0f;
}

static glm::vec3 rotateYaw(const glm::vec3 &d, float yaw) {
	const float c = glm::cos(yaw);
	const float s = glm::sin(yaw);
	return glm::vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

static glm::vec3 latLongToDir(float u, float v) {
	const float theta = v * kPi;
	const float phi = u * 2.0f * kPi;
	const float st = glm::sin(theta);
	return glm::vec3(glm::cos(phi) * st, glm::cos(theta), glm::sin(phi) * st);
}

glm::vec3 VoxelDDAPathTracer::evalEnvironment(const glm::vec3 &dir) const {
	glm::vec3 d = glm::normalize(dir);
	if (_state.hdriEnvironment && _envIsHdri) {
		const glm::vec3 r = rotateYaw(d, -_state.hdriAzimuth);
		return sampleLatLong(_envRgba, _envW, _envH, r) * _state.hdriIntensity;
	}
	if (_state.skyEnvironment) {
		const glm::vec3 sunDir = glm::normalize(glm::vec3(glm::cos(_state.sunAzimuth) * glm::cos(_state.sunElevation),
														  glm::sin(_state.sunElevation),
														  glm::sin(_state.sunAzimuth) * glm::cos(_state.sunElevation)));
		const float ndl = glm::max(0.0f, glm::dot(d, sunDir));
		const glm::vec3 zenith(0.35f, 0.55f, 0.95f);
		const glm::vec3 horizon(0.75f, 0.82f, 0.90f);
		const glm::vec3 sky = glm::mix(horizon, zenith, glm::clamp(d.y * 0.5f + 0.5f, 0.0f, 1.0f));
		return sky + glm::vec3(_state.sunIntensity) * glm::pow(ndl, 256.0f);
	}
	const glm::vec3 base(_state.environmentColor.x, _state.environmentColor.y, _state.environmentColor.z);
	const float v = glm::clamp(0.5f - 0.5f * d.y, 0.0f, 1.0f);
	return base * (1.12f - 0.40f * v);
}

float VoxelDDAPathTracer::environmentPdf(const glm::vec3 &dir) const {
	if (!_envIsHdri || _envCdfSum <= 0.0f || _envW <= 0 || _envH <= 0) {
		return 1.0f / (4.0f * kPi);
	}
	const glm::vec3 d = rotateYaw(glm::normalize(dir), -_state.hdriAzimuth);
	float u = glm::atan(d.z, d.x) / (2.0f * kPi);
	if (u < 0.0f) {
		u += 1.0f;
	}
	const float v = glm::acos(glm::clamp(d.y, -1.0f, 1.0f)) / kPi;
	const int x = glm::clamp((int)(u * (float)_envW), 0, _envW - 1);
	const int y = glm::clamp((int)(v * (float)_envH), 0, _envH - 1);
	const int idx = y * _envW + x;
	const float prev = (idx > 0) ? _envCdf[idx - 1] : 0.0f;
	const float w = _envCdf[idx] - prev;
	const float sinTheta = glm::sin(kPi * ((float)y + 0.5f) / (float)_envH);
	const float angle = (2.0f * kPi / (float)_envW) * (kPi / (float)_envH) * glm::max(sinTheta, 1.0e-8f);
	return (w / _envCdfSum) / angle;
}

bool VoxelDDAPathTracer::sampleHdri(uint32_t sequenceIndex, uint32_t scramble, glm::vec3 &dir,
									glm::vec3 &radiance, float &pdf) const {
	radiance = glm::vec3(0.0f);
	if (!_envIsHdri || _envCdfSum <= 0.0f || _envCdf.empty()) {
		return false;
	}
	const glm::vec2 sequence = sampling::sobol2D(sequenceIndex, scramble);
	const float unit = sampling::sobol1D(sequenceIndex, scramble ^ 0x9e3779b9u);
	const float r = unit * _envCdfSum;
	int lo = 0;
	int hi = _envW * _envH - 1;
	while (lo < hi) {
		const int mid = (lo + hi) / 2;
		if (_envCdf[mid] < r) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	const int idx = lo;
	const int x = idx % _envW;
	const int y = idx / _envW;
	const float u = ((float)x + sequence.x) / (float)_envW;
	const float v = ((float)y + sequence.y) / (float)_envH;
	// Same frame as Yocto: sample in map space, then rotate by +azimuth.
	// evalEnvironment rotates the world dir by -azimuth before lookup.
	dir = glm::normalize(rotateYaw(latLongToDir(u, v), _state.hdriAzimuth));
	const float sinTheta = glm::sin(kPi * ((float)y + 0.5f) / (float)_envH);
	const float angle = (2.0f * kPi / (float)_envW) * (kPi / (float)_envH) * glm::max(sinTheta, 1.0e-8f);
	const float prev = (idx > 0) ? _envCdf[idx - 1] : 0.0f;
	const float wgt = _envCdf[idx] - prev;
	pdf = (wgt / _envCdfSum) / angle;
	const int p = idx * 4;
	radiance = glm::vec3(_envRgba[p], _envRgba[p + 1], _envRgba[p + 2]) * _state.hdriIntensity;
	return pdf > 0.0f;
}

bool VoxelDDAPathTracer::sampleEnvironment(const glm::vec3 &n, uint32_t sequenceIndex, uint32_t scramble,
										   glm::vec3 &dir, glm::vec3 &radiance, float &pdf) const {
	if (_envIsHdri && _envCdfSum > 0.0f) {
		if (!sampleHdri(sequenceIndex, scramble, dir, radiance, pdf)) {
			return false;
		}
		return glm::dot(n, dir) > 0.0f && pdf > 0.0f;
	}
	dir = cosineHemisphere(n, sampling::sobol2D(sequenceIndex, scramble));
	pdf = glm::max(0.0f, glm::dot(n, dir)) * kInvPi;
	radiance = evalEnvironment(dir);
	return pdf > 0.0f;
}

bool VoxelDDAPathTracer::sampleEnvironmentIso(uint32_t sequenceIndex, uint32_t scramble, glm::vec3 &dir,
											  glm::vec3 &radiance, float &pdf) const {
	if (_envIsHdri && _envCdfSum > 0.0f) {
		return sampleHdri(sequenceIndex, scramble, dir, radiance, pdf);
	}
	dir = sampleUniformSphere(sampling::sobol2D(sequenceIndex, scramble));
	pdf = 1.0f / (4.0f * kPi);
	radiance = evalEnvironment(dir);
	return pdf > 0.0f;
}

bool VoxelDDAPathTracer::pointInVoxel(const glm::vec3 &worldPos) const {
	for (const PathTracerGrid &grid : _scene.grids) {
		const glm::vec3 local = glm::vec3(grid.invWorldMat * glm::vec4(worldPos, 1.0f)) + grid.pivot();
		const glm::ivec3 cell((int)glm::floor(local.x), (int)glm::floor(local.y), (int)glm::floor(local.z));
		if (!grid.contains(cell)) {
			continue;
		}
		const uint32_t packed = _scene.cell(grid, grid.localCellIndex(cell));
		if (packed != 0 && isSolidSurf(_scene.material(grid, (uint8_t)(packed - 1u)).surfaceType())) {
			return true;
		}
	}
	return false;
}

bool VoxelDDAPathTracer::sampleMedia(const glm::vec3 &worldPos, glm::vec3 &albedo, float &density, float &rimLight,
									 float &scatter, glm::vec3 &emit) const {
	for (const PathTracerGrid &grid : _scene.grids) {
		const glm::vec3 local = glm::vec3(grid.invWorldMat * glm::vec4(worldPos, 1.0f)) + grid.pivot();
		const glm::ivec3 cell((int)glm::floor(local.x), (int)glm::floor(local.y), (int)glm::floor(local.z));
		if (!grid.contains(cell)) {
			continue;
		}
		const uint32_t packed = _scene.cell(grid, grid.localCellIndex(cell));
		if (packed == 0) {
			continue;
		}
		const uint8_t color = (uint8_t)(packed - 1);
		const PathTracerMaterial &material = _scene.material(grid, color);
		if (material.surfaceType() != kSurfMedia) {
			continue;
		}
		albedo = material.albedo();
		density = material.density();
		rimLight = material.rimLight();
		scatter = material.scatter();
		emit = material.volumeEmission();
		return true;
	}
	return false;
}

glm::vec3 VoxelDDAPathTracer::fieldMediaT(const glm::vec3 &orig, const glm::vec3 &dir, float tmax) const {
	if (!_hasMedia) {
		return glm::vec3(1.0f);
	}
	glm::vec3 T(1.0f);
	const glm::vec3 ndir = glm::normalize(dir);
	const float reach = glm::min(tmax, 64.0f);
	const int n = (int)(reach / kMediaStep) + 1;
	for (int i = 0; i < n; ++i) {
		const float t = ((float)i + 0.5f) * kMediaStep;
		if (t >= reach) {
			break;
		}
		glm::vec3 albedo(0.0f);
		float density = 0.0f;
		float rimLight = 0.0f;
		float scatter = 0.0f;
		glm::vec3 unusedEmit(0.0f);
		if (!sampleMedia(orig + ndir * t, albedo, density, rimLight, scatter, unusedEmit)) {
			continue;
		}
		T *= glm::exp(-volumeSigmaT(density) * kMediaStep);
		if (glm::max(T.x, glm::max(T.y, T.z)) < 1.0e-4f) {
			return glm::vec3(0.0f);
		}
	}
	return T;
}

bool VoxelDDAPathTracer::occluded(const glm::vec3 &orig, const glm::vec3 &dir, bool fromGround, int skipGrid,
								  const glm::ivec3 &skipCell) const {
	if (fromGround && pointInVoxel(orig)) {
		return true;
	}
	for (int i = 0; i < (int)_scene.grids.size(); ++i) {
		Hit h;
		if (traceGrid(_scene.grids[i], i, orig, dir, 1.0e30f, skipGrid, skipCell, true, h) && h.hit) {
			return true;
		}
	}
	return false;
}

static glm::vec3 beer(const glm::vec3 &albedo, float opacity, float atten, float thick) {
	const float o = glm::clamp(opacity, 0.0f, 1.0f);
	const float pass = glm::max(1.0f - o, 1.0e-4f);
	float sigma = -glm::log(pass);
	sigma += glm::clamp(atten, 0.0f, 1.0f) * 4.0f;
	const glm::vec3 stain = glm::mix(glm::vec3(1.0f), albedo, o);
	return stain * glm::exp(-sigma * glm::max(thick, 0.0f));
}

glm::vec3 VoxelDDAPathTracer::shadowTransmittance(const glm::vec3 &orig, const glm::vec3 &dir, float tmax, int skipGrid,
												  const glm::ivec3 &skipCell, int stopGrid,
												  const glm::ivec3 &stopCell) const {
	glm::vec3 T(1.0f);
	const glm::vec3 ndir = glm::normalize(dir);
	if (skipGrid < 0 && pointInVoxel(orig)) {
		return glm::vec3(0.0f);
	}
	for (int gi = 0; gi < (int)_scene.grids.size(); ++gi) {
		const PathTracerGrid &grid = _scene.grids[gi];
		const glm::ivec3 gridMins = grid.mins();
		const glm::ivec3 gridSize = grid.size();
		const glm::vec3 o = glm::vec3(grid.invWorldMat * glm::vec4(orig, 1.0f)) + grid.pivot();
		glm::vec3 d = glm::mat3(grid.invWorldMat) * ndir;
		const float dlen2 = glm::length2(d);
		if (dlen2 < 1.0e-16f) {
			continue;
		}
		d /= glm::sqrt(dlen2);
		const float worldPerLocal = glm::max(glm::length(glm::mat3(grid.worldMat) * d), 1.0e-8f);

		const glm::vec3 bmin(gridMins);
		const glm::vec3 bmax = bmin + glm::vec3(gridSize);
		float t0 = 0.0f;
		float t1 = 1.0e30f;
		int entryAxis = 0;
		bool miss = false;
		for (int a = 0; a < 3; ++a) {
			if (glm::abs(d[a]) < 1.0e-8f) {
				if (o[a] < bmin[a] || o[a] >= bmax[a]) {
					miss = true;
					break;
				}
				continue;
			}
			const float invD = 1.0f / d[a];
			float tNear = (bmin[a] - o[a]) * invD;
			float tFar = (bmax[a] - o[a]) * invD;
			if (tNear > tFar) {
				const float tmp = tNear;
				tNear = tFar;
				tFar = tmp;
			}
			if (tNear > t0) {
				t0 = tNear;
				entryAxis = a;
			}
			t1 = glm::min(t1, tFar);
			if (t0 > t1) {
				miss = true;
				break;
			}
		}
		if (miss || t1 < 0.0f) {
			continue;
		}
		if (t0 < 0.0f) {
			t0 = 0.0f;
		}

		glm::vec3 p = o + d * (t0 + 1.0e-4f);
		const glm::ivec3 cellMax = gridMins + gridSize - 1;
		glm::ivec3 cell(glm::clamp((int)glm::floor(p.x), gridMins.x, cellMax.x),
						glm::clamp((int)glm::floor(p.y), gridMins.y, cellMax.y),
						glm::clamp((int)glm::floor(p.z), gridMins.z, cellMax.z));
		const glm::ivec3 step(d.x >= 0.0f ? 1 : -1, d.y >= 0.0f ? 1 : -1, d.z >= 0.0f ? 1 : -1);
		const float adx = glm::abs(d.x);
		const float ady = glm::abs(d.y);
		const float adz = glm::abs(d.z);
		const glm::vec3 tDelta(adx < 1.0e-8f ? 1.0e30f : 1.0f / adx, ady < 1.0e-8f ? 1.0e30f : 1.0f / ady,
							   adz < 1.0e-8f ? 1.0e30f : 1.0f / adz);
		glm::vec3 tMax;
		tMax.x = adx < 1.0e-8f ? 1.0e30f : ((d.x >= 0.0f ? (float)(cell.x + 1) - p.x : p.x - (float)cell.x) * tDelta.x);
		tMax.y = ady < 1.0e-8f ? 1.0e30f : ((d.y >= 0.0f ? (float)(cell.y + 1) - p.y : p.y - (float)cell.y) * tDelta.y);
		tMax.z = adz < 1.0e-8f ? 1.0e30f : ((d.z >= 0.0f ? (float)(cell.z + 1) - p.z : p.z - (float)cell.z) * tDelta.z);
		float tEnter = t0;
		const int maxSteps = gridSize.x + gridSize.y + gridSize.z + 8;
		(void)entryAxis;
		for (int stepIdx = 0; stepIdx < maxSteps; ++stepIdx) {
			if (!grid.contains(cell)) {
				break;
			}
			const uint32_t packed = _scene.cell(grid, grid.localCellIndex(cell));
			const float tExit = t0 + glm::min(tMax.x, glm::min(tMax.y, tMax.z));
			const float worldEnter = tEnter * worldPerLocal;
			const float worldExit = tExit * worldPerLocal;
			if (worldEnter >= tmax) {
				break;
			}
			if (packed != 0 && !(gi == skipGrid && cell == skipCell)) {
				if (gi == stopGrid && cell == stopCell) {
					return T * fieldMediaT(orig, ndir, tmax);
				}
				const uint8_t color = (uint8_t)(packed - 1);
				const PathTracerMaterial &material = _scene.material(grid, color);
				if (isSolidSurf(material.surfaceType())) {
					return glm::vec3(0.0f);
				}
				const float thick = glm::max(glm::min(worldExit, tmax) - worldEnter, 0.0f);
				if (material.surfaceType() == kSurfMedia) {
					// Media extinction is fieldMediaT, not per DDA cell.
				} else {
					T *= beer(material.albedo(), material.opacity(), material.attenuation(), thick);
				}
				if (glm::max(T.x, glm::max(T.y, T.z)) < 1.0e-4f) {
					return glm::vec3(0.0f);
				}
			}
			if (worldExit >= tmax) {
				break;
			}
			if (tMax.x < tMax.y) {
				if (tMax.x < tMax.z) {
					tEnter = t0 + tMax.x;
					cell.x += step.x;
					tMax.x += tDelta.x;
				} else {
					tEnter = t0 + tMax.z;
					cell.z += step.z;
					tMax.z += tDelta.z;
				}
			} else {
				if (tMax.y < tMax.z) {
					tEnter = t0 + tMax.y;
					cell.y += step.y;
					tMax.y += tDelta.y;
				} else {
					tEnter = t0 + tMax.z;
					cell.z += step.z;
					tMax.z += tDelta.z;
				}
			}
		}
	}
	return T * fieldMediaT(orig, ndir, tmax);
}

glm::vec3 VoxelDDAPathTracer::marchVolume(const glm::vec3 &orig, const glm::vec3 &dir, float tmax, int skipGrid,
										  const glm::ivec3 &skipCell, glm::vec3 &color, const glm::vec3 &throughput,
										  uint32_t sequenceIndex, uint32_t scramble) const {
	glm::vec3 T(1.0f);
	const glm::vec3 ndir = glm::normalize(dir);
	// ANCHOR: Fog is a density field sampled along the ray. Do not
	// accumulate per DDA cell, use occupancy cubes, or dual-axis cell
	// steps. Those three each painted the voxel grid (walls) or 45-degree
	// creases into the interior of a filled region. The outer silhouette
	// stays blocky because occupancy is still cubes; the inside must not.
	if (!_hasMedia) {
		return glm::vec3(1.0f);
	}
	(void)skipGrid;
	(void)skipCell;
	glm::vec3 keyDir(0.0f, 1.0f, 0.0f);
	if (_hasCamera) {
		keyDir = glm::normalize(_camera.forward() * 0.70f + _camera.up() * 0.55f + _camera.right() * 0.30f);
	} else {
		keyDir = glm::normalize(glm::vec3(0.35f, 0.80f, 0.50f));
	}
	glm::vec3 envDir(0.0f);
	glm::vec3 envRad(0.0f);
	float envPdf = 0.0f;
	const bool haveHdri = _envIsHdri && sampleEnvironmentIso(sequenceIndex, scramble ^ 0x94d049bbu, envDir, envRad,
															  envPdf) && envPdf > 1.0e-8f;
	const float reach = glm::min(tmax, 64.0f);
	const float t0 = sampling::sobol1D(sequenceIndex, scramble ^ 0xd1b54a35u) * kMediaStep;
	const int n = (int)(reach / kMediaStep) + 2;
	for (int i = 0; i < n; ++i) {
		const float t = t0 + (float)i * kMediaStep;
		if (t >= reach) {
			break;
		}
		glm::vec3 albedo(0.0f);
		float density = 0.0f;
		float rimLight = 0.0f;
		float scatter = 0.0f;
		glm::vec3 emit(0.0f);
		if (!sampleMedia(orig + ndir * t, albedo, density, rimLight, scatter, emit)) {
			continue;
		}
		const float sig = volumeSigmaT(density);
		const float Tseg = glm::exp(-sig * kMediaStep);
		const float absorbed = 1.0f - Tseg;
		if (glm::max(emit.x, glm::max(emit.y, emit.z)) > 0.0f) {
			// Approach the volume radiance with optical depth so a thick
			// fire saturates instead of stacking lamp-bright steps.
			color += throughput * T * emit * absorbed;
		}
		if (scatter > 1.0e-4f) {
			const glm::vec3 mist = albedo * scatter * absorbed;
			// Studio and HDRI both use this fill. A dark HDRI NEE is too
			// dim (and a recess shadows it) so 0 vs 1 looked identical.
			const glm::vec3 rimDir = haveHdri ? envDir : keyDir;
			const float facing = glm::pow(glm::max(glm::dot(ndir, rimDir), 0.0f), 2.0f);
			const float cloud = scatter * 0.55f * (1.0f + rimLight * 2.5f * facing);
			color += throughput * T * albedo * cloud;
			if (haveHdri) {
				const glm::vec3 Tr = shadowTransmittance(orig + ndir * t, envDir, 1.0e30f, -1, glm::ivec3(0),
														 -1, glm::ivec3(0));
				const float hg = henyeyGreenstein(rimLight, glm::dot(ndir, envDir));
				color += throughput * T * mist * hg * envRad * Tr / envPdf;
			}
			if (!_scene.emitters.empty()) {
				glm::vec3 eldir(0.0f);
				glm::vec3 elrad(0.0f);
				float elpdf = 0.0f;
				if (sampleEmitLight(orig + ndir * t, glm::vec3(0.0f), sequenceIndex + (uint32_t)i,
									scramble ^ 0x369dea0fu, -1, glm::ivec3(0), eldir, elrad, elpdf) &&
					elpdf > 1.0e-8f) {
					const float hg = henyeyGreenstein(rimLight, glm::dot(ndir, eldir));
					color += throughput * T * mist * hg * elrad / elpdf;
				}
			}
		}
		T *= Tseg;
		if (glm::max(T.x, glm::max(T.y, T.z)) < 1.0e-4f) {
			return T;
		}
	}
	return T;
}
bool VoxelDDAPathTracer::traceGrid(const PathTracerGrid &grid, int gridIndex, const glm::vec3 &orig, const glm::vec3 &dir,
								   float tmax, int skipGrid, const glm::ivec3 &skipCell, bool shadowRay,
								   Hit &hit) const {
	(void)grid;
	PathTracerVoxelHit voxelHit;
	if (!pathTracerTraceGrid(_scene, (uint32_t)gridIndex,
			pathTracerRay(orig, dir, tmax, skipGrid, skipCell, shadowRay), voxelHit)) {
		return false;
	}
	const PathTracerGrid &hitGrid = _scene.grids[voxelHit.gridIndex()];
	const PathTracerMaterial &material = _scene.material(hitGrid, voxelHit.materialIndex());
	hit.hit = true;
	hit.ground = false;
	hit.t = voxelHit.positionT.w;
	hit.pos = glm::vec3(voxelHit.positionT);
	hit.normal = glm::vec3(voxelHit.normal);
	hit.localPos = glm::vec3(voxelHit.localPosition);
	hit.localNormal = glm::vec3(voxelHit.localNormal);
	hit.gridIndex = voxelHit.gridIndex();
	hit.cell = voxelHit.cell();
	hit.albedo = material.albedo();
	hit.emit = material.emission();
	hit.opacity = material.opacity();
	hit.ior = material.ior();
	hit.atten = material.attenuation();
	hit.metal = material.metal();
	hit.rough = material.roughness();
	hit.spec = material.specular();
	hit.density = material.density();
	hit.rimLight = material.rimLight();
	hit.scatter = material.scatter();
	hit.surf = material.surfaceType();
	return true;
}

VoxelDDAPathTracer::Hit VoxelDDAPathTracer::trace(const glm::vec3 &orig, const glm::vec3 &dir, int skipGrid,
												 const glm::ivec3 &skipCell, bool shadowRay) const {
	Hit best;
	best.t = 1.0e30f;
	for (int i = 0; i < (int)_scene.grids.size(); ++i) {
		Hit h;
		if (traceGrid(_scene.grids[i], i, orig, dir, best.t, skipGrid, skipCell, shadowRay, h) && h.t < best.t) {
			best = h;
		}
	}
	if (_scene.ground.enabled() && glm::abs(dir.y) > 1.0e-8f) {
		const float t = (_scene.ground.boundsMin.y - orig.y) / dir.y;
		if (t > kEps && t < best.t) {
			const glm::vec3 p = orig + dir * t;
			if (p.x >= _scene.ground.boundsMin.x && p.x <= _scene.ground.boundsMax.x &&
				p.z >= _scene.ground.boundsMin.z && p.z <= _scene.ground.boundsMax.z) {
				best.hit = true;
				best.ground = true;
				best.t = t;
				best.pos = p;
				best.localPos = p;
				best.normal = glm::vec3(0.0f, dir.y < 0.0f ? 1.0f : -1.0f, 0.0f);
				best.localNormal = best.normal;
				best.albedo = glm::vec3(_scene.ground.albedoOpacity);
				best.emit = glm::vec3(0.0f);
			}
		}
	}
	return best;
}

glm::vec4 VoxelDDAPathTracer::tracePath(const glm::vec3 &orig, const glm::vec3 &dir, int sampleIndex,
										uint32_t pixelScramble, glm::vec3 &guideAlbedo, glm::vec3 &guideNormal,
										float &guideDepth, float &guideFeature) const {
	glm::vec3 o = orig;
	glm::vec3 d = glm::normalize(dir);
	glm::vec3 throughput(1.0f);
	glm::vec3 color(0.0f);
	float alpha = 0.0f;
	int skipGrid = -1;
	glm::ivec3 skipCell(0);
	bool specularChain = true;
	bool environmentMisValid = false;
	bool emitterMisValid = false;
	float previousBsdfPdf = 0.0f;
	const int maxBounces = glm::max(1, _state.params.bounces);
	const float clampVal = glm::max(1.0f, _state.params.clamp);
	guideAlbedo = glm::vec3(0.0f);
	guideNormal = glm::vec3(0.0f);
	guideDepth = 0.0f;
	guideFeature = 1.0f;

	for (int b = 0; b < maxBounces; ++b) {
		const uint32_t sequenceIndex = (uint32_t)(sampleIndex * maxBounces + b);
		const uint32_t bounceScramble = pixelScramble ^ ((uint32_t)(b + 1) * 0x9e3779b9u);
		const Hit hit = trace(o, d, skipGrid, skipCell);
		const float tmax = hit.hit ? hit.t : 1.0e30f;
		const glm::vec3 Tvol = marchVolume(o, d, tmax, skipGrid, skipCell, color, throughput, sequenceIndex,
										  bounceScramble ^ 0x7f4a7c15u);
		throughput *= Tvol;
		if (b == 0) {
			const float extinct = 1.0f - (Tvol.x + Tvol.y + Tvol.z) * (1.0f / 3.0f);
			alpha = glm::max(alpha, glm::clamp(extinct, 0.0f, 1.0f));
			glm::vec3 volAlb(0.0f);
			bool haveVol = false;
			float volDepth = 0.0f;
			if (_hasMedia) {
				float vd = 0.0f;
				float vp = 0.0f;
				float vs = 0.0f;
				glm::vec3 ve(0.0f);
				for (int s = 0; s < 12; ++s) {
					const float t = ((float)s + 0.5f) * kMediaStep;
					if (hit.hit && t >= hit.t) {
						break;
					}
					if (sampleMedia(o + d * t, volAlb, vd, vp, vs, ve)) {
						haveVol = true;
						volDepth = t;
						break;
					}
				}
			}
			if (haveVol && extinct > 0.06f) {
				guideAlbedo = volAlb;
				guideNormal = -d;
				guideDepth = volDepth;
			} else if (hit.hit) {
				guideAlbedo = hit.albedo;
				guideNormal = (glm::dot(hit.normal, d) < 0.0f) ? hit.normal : -hit.normal;
				guideDepth = hit.t;
				if (_state.studioEdges && !hit.ground &&
					(hit.surf == kSurfOpaque || hit.surf == kSurfMetal)) {
					guideFeature = studioBevel(hit.localPos, hit.localNormal);
				}
			}
		}
		if (!hit.hit) {
			// Direct environment lighting is evaluated with next-event
			// estimation at every non-delta surface. Only camera and delta
			// paths add a BSDF-hit environment contribution, otherwise the
			// same light is counted twice.
			if (b == 0 || specularChain) {
				color += throughput * evalEnvironment(d);
			} else if (environmentMisValid) {
				const float lightPdf = environmentPdf(d);
				const float weight = powerHeuristic(previousBsdfPdf, lightPdf);
				color += throughput * evalEnvironment(d) * weight;
			}
			if (b == 0 && !_state.params.envhidden) {
				alpha = 1.0f;
				// Miss pixels have no surface. A zero guide normal makes the
				// a-trous pow(n.n, 32) term zero -- including the pixel's own
				// weight -- so denoiseColor writes black over the HDRI.
				if (glm::length2(guideNormal) <= 1.0e-12f) {
					guideAlbedo = evalEnvironment(d);
					guideNormal = -d;
				}
			} else if (b == 0 && _state.params.envhidden && alpha > 1.0e-4f) {
				// RGB is premultiplied in-scatter. ImGui blends rgb*a over
				// the panel; divide so thin fog keeps its albedo instead
				// of washing to grey against the white backdrop.
				color /= alpha;
			}
			break;
		}
		if (b == 0) {
			alpha = 1.0f;
		}
		// Emissive voxels are also sampled explicitly below. Non-delta
		// BSDF hits must not add the same emitter a second time.
		if (b == 0 || specularChain) {
			color += throughput * hit.emit;
		} else if (emitterMisValid && glm::max(hit.emit.x, glm::max(hit.emit.y, hit.emit.z)) > 0.0f) {
			const float lightPdf = emitLightPdf(o, hit);
			const float weight = lightPdf > 0.0f ? powerHeuristic(previousBsdfPdf, lightPdf) : 1.0f;
			color += throughput * hit.emit * weight;
		}

		glm::vec3 albedo = hit.albedo;
		if (_state.studioEdges && !hit.ground && (hit.surf == kSurfOpaque || hit.surf == kSurfMetal)) {
			albedo *= studioBevel(hit.localPos, hit.localNormal);
		}

		if (!hit.ground && hit.surf == kSurfGlass) {
			environmentMisValid = false;
			emitterMisValid = false;
			const bool entering = glm::dot(d, hit.normal) < 0.0f;
			const glm::vec3 ns = entering ? hit.normal : -hit.normal;
			const float ior = glm::max(hit.ior, 1.01f);
			const float eta = entering ? (1.0f / ior) : ior;
			const float cosi = glm::clamp(-glm::dot(d, ns), 0.0f, 1.0f);
			glm::vec3 rd = glm::refract(d, ns, eta);
			const bool tir = glm::dot(rd, rd) < 1.0e-12f;
			const float F = tir ? 1.0f : fresnelSchlick(cosi, ior);
			if (tir || sampling::sobol1D(sequenceIndex, bounceScramble ^ 0x51ed270bu) < F) {
				d = glm::reflect(d, ns);
				o = hit.pos + ns * kSpawnEps;
			} else {
				d = glm::normalize(rd);
				o = hit.pos + d * kSpawnEps;
				throughput *= beer(albedo, hit.opacity, hit.atten, 1.0f);
			}
			skipGrid = hit.gridIndex;
			skipCell = hit.cell;
			continue;
		}

		if (!hit.ground && hit.surf == kSurfAlpha &&
			sampling::sobol1D(sequenceIndex, bounceScramble ^ 0x85ebca6bu) >= hit.opacity) {
			environmentMisValid = false;
			emitterMisValid = false;
			o = hit.pos + d * kSpawnEps;
			skipGrid = hit.gridIndex;
			skipCell = hit.cell;
			throughput *= glm::mix(glm::vec3(1.0f), albedo, hit.opacity * 0.35f);
			continue;
		}

		const glm::vec3 n = (glm::dot(hit.normal, d) < 0.0f) ? hit.normal : -hit.normal;
		const glm::vec3 p = hit.pos + n * kSpawnEps;
		const glm::vec3 wo = -d;
		skipGrid = hit.ground ? -1 : hit.gridIndex;
		skipCell = hit.cell;
		const float metal = hit.ground ? 0.0f : hit.metal;
		const float rough = glm::clamp(hit.rough, 0.04f, 1.0f);
		const float a = rough * rough;
		specularChain = false;

		// Single environment next-event sample with full MIS (1 NEE + 1 BSDF).
		{
			glm::vec3 ldir(0.0f);
			glm::vec3 lrad(0.0f);
			float lpdf = 0.0f;
			if (sampleEnvironment(n, sequenceIndex, bounceScramble ^ 0x68bc21ebu, ldir, lrad, lpdf)) {
				const float ndl = glm::dot(n, ldir);
				if (ndl > 1.0e-6f && lpdf > 0.0f) {
					const glm::vec3 T = shadowTransmittance(p, ldir, 1.0e30f, skipGrid, skipCell, -1, glm::ivec3(0));
					if (glm::max(T.x, glm::max(T.y, T.z)) > 1.0e-5f) {
						const glm::vec3 fcos = evalOpaqueFcos(albedo, metal, hit.spec, a, n, wo, ldir);
						const float bsdfPdf = opaquePdf(metal, hit.spec, albedo, a, n, wo, ldir);
						const float misWeight = _envIsHdri && b + 1 < maxBounces
							? powerHeuristic(lpdf, bsdfPdf)
							: 1.0f;
						color += throughput * fcos * lrad * T * misWeight / lpdf;
					}
				}
			}
		}

		glm::vec3 eldir(0.0f);
		glm::vec3 elrad(0.0f);
		float elpdf = 0.0f;
		if (sampleEmitLight(p, n, sequenceIndex, bounceScramble ^ 0x165667b1u, skipGrid, skipCell, eldir, elrad,
							elpdf)) {
			const float ndl = glm::dot(n, eldir);
			if (ndl > 1.0e-6f && elpdf > 0.0f) {
				const glm::vec3 fcos = evalOpaqueFcos(albedo, metal, hit.spec, a, n, wo, eldir);
				const float bsdfPdf = opaquePdf(metal, hit.spec, albedo, a, n, wo, eldir);
				const float misWeight = b + 1 < maxBounces ? powerHeuristic(elpdf, bsdfPdf) : 1.0f;
				color += throughput * fcos * elrad * misWeight / elpdf;
			}
		}

		const glm::vec3 f0 = glm::mix(glm::vec3(0.04f * (0.2f + 0.8f * hit.spec)), albedo, metal);
		const float ndv = glm::max(glm::dot(n, wo), 0.0f);
		const glm::vec3 Fv = fresnelSchlick3(f0, ndv);
		const float pSpec = glm::clamp(0.25f + 0.75f * (Fv.x + Fv.y + Fv.z) * (1.0f / 3.0f), 0.15f, 0.95f);
		const glm::vec2 bsdfSequence = sampling::sobol2D(sequenceIndex, bounceScramble ^ 0x02e5be93u);
		const float specularChoice = sampling::sobol1D(sequenceIndex, bounceScramble ^ 0x7f4a7c15u);
		glm::vec3 wi(0.0f);
		if (specularChoice < pSpec) {
			const glm::vec3 h = sampleGgxVisibleHalf(n, wo, a, bsdfSequence);
			wi = glm::reflect(-wo, h);
		} else {
			wi = cosineHemisphere(n, bsdfSequence);
		}
		const float pdf = opaquePdf(metal, hit.spec, albedo, a, n, wo, wi);
		if (glm::dot(n, wi) <= 1.0e-6f || pdf <= 1.0e-6f) {
			break;
		}
		throughput *= evalOpaqueFcos(albedo, metal, hit.spec, a, n, wo, wi) / pdf;
		previousBsdfPdf = pdf;
		environmentMisValid = _envIsHdri;
		emitterMisValid = !_scene.emitters.empty();
		d = wi;
		o = p;
		// Unbiased Russian roulette replaces the old hard throughput cutoff,
		// which silently dropped the residual energy of dim paths (darkened
		// deep-indirect corners). Terminate probabilistically and rescale
		// survivors so the expected contribution is unchanged.
		const float throughputMax = glm::max(throughput.x, glm::max(throughput.y, throughput.z));
		if (throughputMax < 0.2f) {
			const float survival = glm::max(throughputMax, 1.0e-4f);
			if (sampling::sobol1D(sequenceIndex, bounceScramble ^ 0x5be0cd19u) > survival) {
				break;
			}
			throughput /= survival;
		}
	}

	const float m = glm::max(color.x, glm::max(color.y, color.z));
	if (m > clampVal) {
		color *= clampVal / m;
	}
	return glm::vec4(color, alpha);
}

void VoxelDDAPathTracer::accumulateSample() {
	const int w = _width;
	const int h = _height;
	const int sample = _sample;
	const bool adaptive = _state.params.adaptive;
	app::for_parallel(0, h, [this, w, h, sample, adaptive](int y0, int y1) {
		for (int y = y0; y < y1; ++y) {
			for (int x = 0; x < w; ++x) {
				const int p = y * w + x;
				const int count = _accumCount[p];
				// Adaptive sampling: a pixel whose mean is already stable no longer
				// receives rays. The Sobol sequence index stays the global pass count,
				// so an active pixel's samples are bit-identical to the non-adaptive
				// schedule; converged pixels simply stop accumulating.
				if (adaptive && count >= kAdaptiveMinSamples && pixelConverged(p)) {
					continue;
				}
				const uint32_t pixelScramble = wangHash((uint32_t)(x + y * w) ^ 0xa511e9b3u);
				const glm::vec2 cameraSample = sampling::sobol2D((uint32_t)sample, pixelScramble);
				const float jx = (float)x + cameraSample.x;
				const float jy = (float)y + cameraSample.y;
				const math::Ray ray = pathTracerCameraRay(_cameraData, jx, jy);
				glm::vec3 guideA(0.0f);
				glm::vec3 guideN(0.0f);
				float guideDepth = 0.0f;
				float guideFeature = 1.0f;
				glm::vec4 c = tracePath(ray.origin, ray.direction, sample, pixelScramble, guideA, guideN, guideDepth,
										guideFeature);
				if (count >= kAdaptiveMinSamples) {
					const float historyScale = 1.0f / (float)count;
					const float mean = (0.2126f * _accum[p * 4 + 0] + 0.7152f * _accum[p * 4 + 1] +
										0.0722f * _accum[p * 4 + 2]) * historyScale;
					const float historyVariance = glm::max(0.0f, _accumLuma2[p] * historyScale - mean * mean);
					const float sampleLuma = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
					// A late, conservative history clamp catches isolated fireflies
					// without locking out a highlight that was absent from the first
					// few samples. The explicit render clamp remains the hard ceiling.
					const float statisticalLimit = mean + 10.0f * glm::sqrt(historyVariance) + 0.25f;
					const float relativeLimit = mean * 6.0f + 0.5f;
					const float historyLimit = glm::max(statisticalLimit, relativeLimit);
					if (sampleLuma > historyLimit) {
						const float adjustment = historyLimit / glm::max(sampleLuma, 1.0e-6f);
						c.x *= adjustment;
						c.y *= adjustment;
						c.z *= adjustment;
					}
				}
				const int i = (y * w + x) * 4;
				_accum[i + 0] += c.x;
				_accum[i + 1] += c.y;
				_accum[i + 2] += c.z;
				_accum[i + 3] += c.w;
				const int g = (y * w + x) * 3;
				_accumAlbedo[g + 0] += guideA.x;
				_accumAlbedo[g + 1] += guideA.y;
				_accumAlbedo[g + 2] += guideA.z;
				_accumNormal[g + 0] += guideN.x;
				_accumNormal[g + 1] += guideN.y;
				_accumNormal[g + 2] += guideN.z;
				_accumDepth[p] += guideDepth;
				_accumFeature[p] += guideFeature;
				const float luma = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
				_accumLuma2[p] += luma * luma;
				_accumCount[p] = count + 1;
			}
		}
	});
	++_sample;
}

bool VoxelDDAPathTracer::pixelConverged(int p) const {
	const int count = _accumCount[p];
	if (count < kAdaptiveMinSamples) {
		return false;
	}
	const float inv = 1.0f / (float)count;
	const float mean = (0.2126f * _accum[p * 4 + 0] + 0.7152f * _accum[p * 4 + 1] + 0.0722f * _accum[p * 4 + 2]) *
					   inv;
	const float variance = glm::max(0.0f, _accumLuma2[p] * inv - mean * mean);
	const float stdErr = glm::sqrt(variance * inv);
	const float relErr = stdErr / glm::max(mean, kAdaptiveNoiseFloor);
	return relErr <= _state.params.adaptiveError;
}

bool VoxelDDAPathTracer::allPixelsConverged() const {
	if (!_state.params.adaptive) {
		return false;
	}
	const int n = _width * _height;
	for (int p = 0; p < n; ++p) {
		if (!pixelConverged(p)) {
			return false;
		}
	}
	return true;
}

bool VoxelDDAPathTracer::start(const scenegraph::SceneGraph &sceneGraph, const video::Camera *camera) {
	applyAppearanceFromScene(sceneGraph);
	Log::info("DDA start: hdriEnv=%s hdriPath='%s' envhidden=%s filmic=%s",
		_state.hdriEnvironment ? "true" : "false",
		_state.hdriPath.c_str(),
		_state.params.envhidden ? "true" : "false",
		_state.filmic ? "true" : "false");
	if (_state.hdriEnvironment && !_state.hdriPath.empty()) {
		if (!loadHdri(_state.hdriPath)) {
			Log::warn("HDRI disabled after failed load of '%s'", _state.hdriPath.c_str());
			_state.hdriEnvironment = false;
		} else {
			Log::info("HDRI loaded: %dx%d cdfSum=%f", _envW, _envH, (double)_envCdfSum);
		}
	} else {
		_envIsHdri = false;
		_envRgba.release();
		_envCdf.release();
		_envCdfSum = 0.0f;
	}

	buildGrids(sceneGraph);
	_state.backendMessage = "";
#ifdef __EMSCRIPTEN__
	_webGPULifecycle = PathTracerWebGPULifecycle();
	_webGPUEnabled = !_scene.grids.empty() && _webGPU.init();
	if (_scene.grids.empty()) {
		_state.backendMessage = "WebGPU path tracer has no voxel grids; using CPU renderer";
		Log::warn("%s", _state.backendMessage.c_str());
	} else if (!_webGPUEnabled) {
		_state.backendMessage = "WebGPU is unavailable; using CPU path tracer";
		Log::warn("%s", _state.backendMessage.c_str());
	}
	_webGPUScenePending = _webGPUEnabled;
	_webGPUEnvironmentPending = _webGPUScenePending && _envIsHdri && !_envRgba.empty() && !_envCdf.empty() &&
		_envW > 0 && _envH > 0 && _envCdfSum > 0.0f;
	_webGPUDispatchPending = false;
#endif

	_hasCamera = camera != nullptr;
	if (camera != nullptr) {
		_camera = *camera;
		_camera.update(0.0);
	} else {
		_camera = video::Camera();
		_camera.setSize(glm::ivec2(512, 512));
		_camera.setWorldPosition(glm::vec3(32.0f, 32.0f, 32.0f));
		_camera.lookAt(glm::vec3(0.0f));
		_camera.update(0.0);
		_hasCamera = true;
	}

	const glm::ivec2 camSize = glm::max(_camera.size(), glm::ivec2(1, 1));
	const float aspect = (float)camSize.x / (float)camSize.y;
	_width = glm::max(16, _state.params.resolution);
	_height = glm::max(16, (int)((float)_width / aspect + 0.5f));
	video::Camera rayCamera = _camera;
	rayCamera.setSize(glm::ivec2(_width, _height));
	rayCamera.update(0.0);
	_cameraData = pathTracerCameraData(rayCamera);
	_accum.resize((size_t)_width * (size_t)_height * 4u);
	core_memset(_accum.data(), 0, _accum.size() * sizeof(float));
	_accumAlbedo.resize((size_t)_width * (size_t)_height * 3u);
	core_memset(_accumAlbedo.data(), 0, _accumAlbedo.size() * sizeof(float));
	_accumNormal.resize((size_t)_width * (size_t)_height * 3u);
	core_memset(_accumNormal.data(), 0, _accumNormal.size() * sizeof(float));
	_accumDepth.resize((size_t)_width * (size_t)_height);
	core_memset(_accumDepth.data(), 0, _accumDepth.size() * sizeof(float));
	_accumLuma2.resize((size_t)_width * (size_t)_height);
	core_memset(_accumLuma2.data(), 0, _accumLuma2.size() * sizeof(float));
	_accumFeature.resize((size_t)_width * (size_t)_height);
	core_memset(_accumFeature.data(), 0, _accumFeature.size() * sizeof(float));
	_accumCount.resize((size_t)_width * (size_t)_height);
	core_memset(_accumCount.data(), 0, _accumCount.size() * sizeof(int));
#ifdef __EMSCRIPTEN__
	_webGPUImage.resize((size_t)_width * (size_t)_height * 4u);
	_webGPUConverged = false;
	_webGPUDenoiseSeeded = false;
	_webGPUBatchSamples = 0u;
#endif
	_sample = 0;
	_temporalValid = false;
	_state.started = true;
	Log::debug("Started voxel DDA path tracer %ix%i", _width, _height);
	return true;
}

bool VoxelDDAPathTracer::restart(const scenegraph::SceneGraph &sceneGraph, const video::Camera *camera) {
	if (started()) {
		stop();
	}
	return start(sceneGraph, camera);
}

bool VoxelDDAPathTracer::stop() {
	_state.started = false;
#ifdef __EMSCRIPTEN__
	_webGPU.abort();
	_webGPUEnabled = false;
	_webGPUScenePending = false;
	_webGPUEnvironmentPending = false;
	_webGPUDispatchPending = false;
	_webGPULifecycle.enabled = false;
#endif
	return true;
}

bool VoxelDDAPathTracer::started() const {
	return _state.started;
}

bool VoxelDDAPathTracer::update(int *currentSample) {
	if (!_state.started) {
		if (currentSample) {
			*currentSample = 0;
		}
		return true;
	}
	const int target = glm::max(1, _state.params.samples);
	const int batch = glm::max(1, _state.params.batch);
#ifdef __EMSCRIPTEN__
	if (_webGPUEnabled) {
		_webGPU.update();
		auto applyBackendEvent = [this](PathTracerWebGPUEvent event) {
			const bool keepGpu = pathTracerWebGPUApplyEvent(_webGPULifecycle, event);
			if (!_webGPULifecycle.message.empty()) {
				_state.backendMessage = _webGPULifecycle.message;
				Log::warn("%s at sample %i", _state.backendMessage.c_str(), _sample);
			}
			if (_webGPULifecycle.resetAccumulation) {
				if (!_accum.empty()) {
					core_memset(_accum.data(), 0, _accum.size() * sizeof(float));
				}
				if (!_accumAlbedo.empty()) {
					core_memset(_accumAlbedo.data(), 0, _accumAlbedo.size() * sizeof(float));
				}
				if (!_accumNormal.empty()) {
					core_memset(_accumNormal.data(), 0, _accumNormal.size() * sizeof(float));
				}
				if (!_accumDepth.empty()) {
					core_memset(_accumDepth.data(), 0, _accumDepth.size() * sizeof(float));
				}
				if (!_accumLuma2.empty()) {
					core_memset(_accumLuma2.data(), 0, _accumLuma2.size() * sizeof(float));
				}
				if (!_accumFeature.empty()) {
					core_memset(_accumFeature.data(), 0, _accumFeature.size() * sizeof(float));
				}
				if (!_accumCount.empty()) {
					core_memset(_accumCount.data(), 0, _accumCount.size() * sizeof(int));
				}
				_sample = 0;
			}
			if (!keepGpu) {
				_webGPUEnabled = false;
				_webGPUScenePending = false;
				_webGPUEnvironmentPending = false;
				_webGPUDispatchPending = false;
			}
			return keepGpu;
		};
		if (_webGPU.needsUpload()) {
			applyBackendEvent(PathTracerWebGPUEvent::Recovered);
			_webGPUScenePending = true;
			_webGPUEnvironmentPending = _envIsHdri && !_envRgba.empty() && !_envCdf.empty() &&
				_envW > 0 && _envH > 0 && _envCdfSum > 0.0f;
			_webGPUDispatchPending = false;
		}
		if (_webGPUScenePending && _webGPU.ready()) {
			_webGPUScenePending = !_webGPU.uploadScene(_scene);
		}
		if (!_webGPUScenePending && _webGPUEnvironmentPending && _webGPU.ready()) {
			const bool uploaded = _webGPU.uploadEnvironment(_envRgba.data(), _envCdf.data(),
					static_cast<uint32_t>(_envW), static_cast<uint32_t>(_envH), _envCdfSum);
			_webGPUEnvironmentPending = !uploaded;
			Log::info("WebGPU env upload: %s (w=%i h=%i cdfSum=%f)",
				uploaded ? "ok" : "failed", _envW, _envH, (double)_envCdfSum);
		}
		if (_webGPU.state() == PathTracerWebGPUState::Failed) {
			const core::String &jsMessage = _webGPU.fallbackMessage();
			if (jsMessage.contains("memory")) {
				applyBackendEvent(PathTracerWebGPUEvent::ResourceExhausted);
			} else if (jsMessage.contains("readback")) {
				applyBackendEvent(PathTracerWebGPUEvent::ReadbackFailed);
			} else if (jsMessage.contains("dispatch")) {
				applyBackendEvent(PathTracerWebGPUEvent::DispatchRejected);
			} else {
				applyBackendEvent(PathTracerWebGPUEvent::RecoverFailed);
			}
		}
		if (_webGPUEnabled && _webGPUDispatchPending) {
			uint32_t pixelCount = 0u;
			uint32_t unconverged = 0u;
			if (_webGPU.takeBatch(_webGPUImage.data(), static_cast<uint32_t>(_webGPUImage.size()),
					pixelCount, unconverged)) {
				const uint32_t expectedPixels = static_cast<uint32_t>(_width * _height);
				if (pixelCount == expectedPixels) {
					if (_sample == 0) {
						Log::info("WebGPU path tracer active");
					}
					_sample += static_cast<int>(_webGPUBatchSamples);
					_webGPUConverged = unconverged == 0u;
					_webGPUDenoiseSeeded = true;
					_webGPUDispatchPending = false;
					_webGPULifecycle.invalidRetry = false;
					// Check if the batch produced all-black output
					bool allBlack = true;
					const uint8_t *bytes = _webGPUImage.data();
					for (uint32_t i = 0; i < pixelCount; ++i) {
						if (bytes[i * 4 + 3] > 0u) {
							allBlack = false;
							break;
						}
					}
					Log::info("WebGPU batch sample=%i pixels=%u unconverged=%u allBlack=%s first=%i,%i,%i,%i",
							_sample, pixelCount, unconverged, allBlack ? "true" : "false",
							(int)bytes[0], (int)bytes[1], (int)bytes[2], (int)bytes[3]);
				} else {
					Log::warn("WebGPU returned invalid batch data at sample %i", _sample);
					_webGPUDispatchPending = false;
					applyBackendEvent(PathTracerWebGPUEvent::InvalidAccumulation);
				}
			}
		}
		if (_webGPUEnabled && !_webGPUScenePending && !_webGPUEnvironmentPending &&
			!_webGPUDispatchPending && _sample < target && !_webGPUConverged) {
			PathTracerPrimaryParams params;
			params.pixelCount = static_cast<uint32_t>(_width * _height);
			params.sampleIndex = static_cast<uint32_t>(_sample);
			params.adaptiveEnabled = _state.params.adaptive ? 1u : 0u;
			params.adaptiveError = _state.params.adaptiveError;
			params.adaptiveMinSamples = static_cast<uint32_t>(kAdaptiveMinSamples);
			const uint32_t sampleCount = static_cast<uint32_t>(glm::min(batch, target - _sample));
			const PathTracerLightingData lighting = webGPULightingData(_state, _envIsHdri);
			Log::info("WebGPU dispatch: flags=(%u,%u,%u,%u) envColor=(%f,%f,%f) w=%i h=%i",
				lighting.flags.x, lighting.flags.y, lighting.flags.z, lighting.flags.w,
				(double)lighting.environmentColor.x, (double)lighting.environmentColor.y, (double)lighting.environmentColor.z,
				_width, _height);
			if (_webGPU.renderBatch(_cameraData, params, lighting, sampleCount,
					static_cast<uint32_t>(_width), static_cast<uint32_t>(_height), _state.exposure,
					_state.filmic, !_webGPUDenoiseSeeded)) {
				_webGPUBatchSamples = sampleCount;
				_webGPUDispatchPending = true;
			} else if (_webGPU.state() == PathTracerWebGPUState::Failed) {
				const core::String &jsMessage = _webGPU.fallbackMessage();
				if (jsMessage.contains("memory")) {
					applyBackendEvent(PathTracerWebGPUEvent::ResourceExhausted);
				} else {
					applyBackendEvent(PathTracerWebGPUEvent::DispatchRejected);
				}
			} else {
				applyBackendEvent(PathTracerWebGPUEvent::DispatchRejected);
			}
		}
		if (_webGPUEnabled) {
			if (currentSample) {
				*currentSample = _sample;
			}
			if ((_sample >= target || _webGPUConverged) && !_webGPUDispatchPending) {
				_state.started = false;
				return true;
			}
			return false;
		}
	}
#endif
	for (int i = 0; i < batch && _sample < target; ++i) {
		accumulateSample();
	}
	if (currentSample) {
		*currentSample = _sample;
	}
	if (_sample >= target || allPixelsConverged()) {
		_state.started = false;
		return true;
	}
	return false;
}

void VoxelDDAPathTracer::denoiseColor(float *rgb) {
	const int w = _width;
	const int h = _height;
	const int n = w * h;
	if (n <= 0 || _accumAlbedo.size() < (size_t)n * 3u || _accumNormal.size() < (size_t)n * 3u ||
		_accumDepth.size() < (size_t)n || _accumLuma2.size() < (size_t)n || _accumFeature.size() < (size_t)n ||
		_accumCount.size() < (size_t)n) {
		return;
	}
	core::Buffer<float> albedo;
	core::Buffer<float> normal;
	core::Buffer<float> alpha;
	core::Buffer<float> depth;
	core::Buffer<float> variance;
	core::Buffer<float> feature;
	albedo.resize((size_t)n * 3u);
	normal.resize((size_t)n * 3u);
	alpha.resize((size_t)n);
	depth.resize((size_t)n);
	variance.resize((size_t)n);
	feature.resize((size_t)n);
	for (int i = 0; i < n; ++i) {
		// Adaptive sampling gives each pixel its own sample count, so the mean and
		// variance are normalized per pixel rather than by the global pass count.
		const float scale = 1.0f / (float)glm::max(_accumCount[i], 1);
		albedo[i * 3 + 0] = _accumAlbedo[i * 3 + 0] * scale;
		albedo[i * 3 + 1] = _accumAlbedo[i * 3 + 1] * scale;
		albedo[i * 3 + 2] = _accumAlbedo[i * 3 + 2] * scale;
		glm::vec3 nn(_accumNormal[i * 3 + 0], _accumNormal[i * 3 + 1], _accumNormal[i * 3 + 2]);
		const float nlen = glm::length(nn);
		if (nlen > 1.0e-6f) {
			nn /= nlen;
		}
		normal[i * 3 + 0] = nn.x;
		normal[i * 3 + 1] = nn.y;
		normal[i * 3 + 2] = nn.z;
		alpha[i] = glm::clamp(_accum[i * 4 + 3] * scale, 0.0f, 1.0f);
		depth[i] = _accumDepth[i] * scale;
		const float luma = 0.2126f * rgb[i * 3 + 0] + 0.7152f * rgb[i * 3 + 1] + 0.0722f * rgb[i * 3 + 2];
		const float sampleVariance = glm::max(0.0f, _accumLuma2[i] * scale - luma * luma);
		variance[i] = sampleVariance * scale;
		feature[i] = _accumFeature[i] * scale;
	}

	// Edge-aware a-trous filter. Albedo, normal, depth, and alpha stop the
	// kernel at geometry boundaries. The luminance moment controls how far
	// noisy pixels may borrow while preserving converged shadows and highlights.
	// A feature barrier prevents wider passes from stepping over a voxel seam
	// when both sample endpoints happen to lie in the interiors of adjacent voxels.
	core::Buffer<float> src;
	core::Buffer<float> dst;
	src.resize((size_t)n * 3u);
	dst.resize((size_t)n * 3u);
	// The studio voxel seam is analytic primary-surface shading. Remove it
	// from the signal being filtered and put it back after the a-trous passes;
	// otherwise any spatial denoiser eventually averages the seam away. The
	// feature buffer is accumulated from the same camera samples as radiance,
	// so antialiased seam coverage is retained.
	for (int i = 0; i < n; ++i) {
		const float edgeFactor = glm::clamp(feature[i], 0.62f, 1.02f);
		const float invEdgeFactor = 1.0f / edgeFactor;
		src[i * 3 + 0] = rgb[i * 3 + 0] * invEdgeFactor;
		src[i * 3 + 1] = rgb[i * 3 + 1] * invEdgeFactor;
		src[i * 3 + 2] = rgb[i * 3 + 2] * invEdgeFactor;
	}
	const int kernel[3] = {1, 2, 1};
	for (int pass = 0; pass < 3; ++pass) {
		const int step = 1 << pass;
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				const int i = y * w + x;
				if (alpha[i] < 1.0e-3f) {
					dst[i * 3 + 0] = src[i * 3 + 0];
					dst[i * 3 + 1] = src[i * 3 + 1];
					dst[i * 3 + 2] = src[i * 3 + 2];
					continue;
				}
				const float *a0 = albedo.data() + i * 3;
				const float *n0 = normal.data() + i * 3;
				const float *c0 = src.data() + i * 3;
				const float l0 = 0.2126f * c0[0] + 0.7152f * c0[1] + 0.0722f * c0[2];
				glm::vec3 sum(0.0f);
				float sumWeight = 0.0f;
				for (int ky = -1; ky <= 1; ++ky) {
					for (int kx = -1; kx <= 1; ++kx) {
						const int nx = x + kx * step;
						const int ny = y + ky * step;
						if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
							continue;
						}
						const int j = ny * w + nx;
						const float *a1 = albedo.data() + j * 3;
						const float *n1 = normal.data() + j * 3;
						const float *c1 = src.data() + j * 3;
						const float da0 = a0[0] - a1[0];
						const float da1 = a0[1] - a1[1];
						const float da2 = a0[2] - a1[2];
						const float albedoWeight = glm::exp(-(da0 * da0 + da1 * da1 + da2 * da2) * 80.0f);
						const float nlen0 = n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2];
						const float nlen1 = n1[0] * n1[0] + n1[1] * n1[1] + n1[2] * n1[2];
						float normalWeight = 1.0f;
						if (nlen0 > 1.0e-12f && nlen1 > 1.0e-12f) {
							const float ndot = glm::clamp(n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2], 0.0f, 1.0f);
							normalWeight = glm::pow(ndot, 32.0f);
						} else if (nlen0 > 1.0e-12f || nlen1 > 1.0e-12f) {
							normalWeight = 0.0f;
						}
						const float depthScale = 0.03f * glm::max(depth[i], depth[j]) + 0.01f;
						const float depthDelta = glm::abs(depth[i] - depth[j]) / depthScale;
						const float depthWeight = glm::exp(-depthDelta * depthDelta);
						const float featureDelta = feature[i] - feature[j];
						const float featureWeight = glm::exp(-featureDelta * featureDelta * 1000.0f);
						float featureBarrierWeight = 1.0f;
						if (step > 1 && (kx != 0 || ky != 0)) {
							float minFeature = 1.0f;
							for (int s = 1; s < step; ++s) {
								const int sx = x + kx * s;
								const int sy = y + ky * s;
								minFeature = glm::min(minFeature, feature[sy * w + sx]);
							}
							const float barrier = 1.0f - minFeature;
							featureBarrierWeight = glm::exp(-barrier * barrier * 64.0f);
						}
						const float alphaDelta = alpha[i] - alpha[j];
						const float alphaWeight = glm::exp(-alphaDelta * alphaDelta * 64.0f);
						const float l1 = 0.2126f * c1[0] + 0.7152f * c1[1] + 0.0722f * c1[2];
						const float colorSigma = 1.5f * glm::sqrt(variance[i] + variance[j]) +
										 0.025f * glm::max(l0, l1) + 0.002f;
						const float lumaDelta = (l0 - l1) / colorSigma;
						const float colorWeight = glm::exp(-0.125f * lumaDelta * lumaDelta);
						const float spatialWeight = (float)(kernel[kx + 1] * kernel[ky + 1]);
						const float weight = spatialWeight * albedoWeight * normalWeight * depthWeight * featureWeight *
									 featureBarrierWeight * alphaWeight * colorWeight;
						sum += glm::vec3(c1[0], c1[1], c1[2]) * weight;
						sumWeight += weight;
					}
				}
				const float invWeight = sumWeight > 1.0e-8f ? 1.0f / sumWeight : 1.0f;
				dst[i * 3 + 0] = sum.x * invWeight;
				dst[i * 3 + 1] = sum.y * invWeight;
				dst[i * 3 + 2] = sum.z * invWeight;
			}
		}
		core_memcpy(src.data(), dst.data(), (size_t)n * 3u * sizeof(float));
	}
	for (int i = 0; i < n; ++i) {
		const float edgeFactor = glm::clamp(feature[i], 0.62f, 1.02f);
		rgb[i * 3 + 0] = src[i * 3 + 0] * edgeFactor;
		rgb[i * 3 + 1] = src[i * 3 + 1] * edgeFactor;
		rgb[i * 3 + 2] = src[i * 3 + 2] * edgeFactor;
	}
	denoiseTemporal(rgb, normal.data(), albedo.data(), depth.data(), alpha.data());
}

void VoxelDDAPathTracer::denoiseTemporal(float *rgb, const float *normal, const float *albedo, const float *depth,
										 const float *alpha) {
	const int w = _width;
	const int h = _height;
	const int n = w * h;
	if (n <= 0) {
		return;
	}
	// First temporal frame (or a reset): seed the history from the current
	// spatially-filtered result and return it unchanged.
	const bool seed = !_temporalValid || _temporalColor.size() < (size_t)n * 3u ||
		_temporalCount.size() < (size_t)n;
	if (seed) {
		_temporalColor.resize((size_t)n * 3u);
		_temporalNormal.resize((size_t)n * 3u);
		_temporalAlbedo.resize((size_t)n * 3u);
		_temporalDepth.resize((size_t)n);
		_temporalCount.resize((size_t)n);
		core_memcpy(_temporalColor.data(), rgb, (size_t)n * 3u * sizeof(float));
		core_memcpy(_temporalNormal.data(), normal, (size_t)n * 3u * sizeof(float));
		core_memcpy(_temporalAlbedo.data(), albedo, (size_t)n * 3u * sizeof(float));
		core_memcpy(_temporalDepth.data(), depth, (size_t)n * sizeof(float));
		for (int i = 0; i < n; ++i) {
			_temporalCount[i] = 1.0f;
		}
		_temporalCamera = _cameraData;
		_temporalValid = true;
		return;
	}

	// Motion-vector reprojection: reconstruct each pixel's world position from its
	// depth and project it through the previous frame's view-projection. Identity
	// while the camera is fixed during accumulation.
	const glm::mat4 previousViewProjection = glm::inverse(_temporalCamera.inverseViewProjection);

	core::Buffer<float> nextColor;
	core::Buffer<float> nextNormal;
	core::Buffer<float> nextAlbedo;
	core::Buffer<float> nextDepth;
	core::Buffer<float> nextCount;
	nextColor.resize((size_t)n * 3u);
	nextNormal.resize((size_t)n * 3u);
	nextAlbedo.resize((size_t)n * 3u);
	nextDepth.resize((size_t)n);
	nextCount.resize((size_t)n);

	const float maxHistory = 32.0f;

	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const int i = y * w + x;
			const int i3 = i * 3;
			// Transparent / background pixels keep the current color and reset
			// history (there is no surface to reproject).
			if (alpha[i] < 1.0e-3f || depth[i] <= 0.0f) {
				nextColor[i3 + 0] = rgb[i3 + 0];
				nextColor[i3 + 1] = rgb[i3 + 1];
				nextColor[i3 + 2] = rgb[i3 + 2];
				nextNormal[i3 + 0] = normal[i3 + 0];
				nextNormal[i3 + 1] = normal[i3 + 1];
				nextNormal[i3 + 2] = normal[i3 + 2];
				nextAlbedo[i3 + 0] = albedo[i3 + 0];
				nextAlbedo[i3 + 1] = albedo[i3 + 1];
				nextAlbedo[i3 + 2] = albedo[i3 + 2];
				nextDepth[i] = depth[i];
				nextCount[i] = 0.0f;
				continue;
			}
			const math::Ray ray = pathTracerCameraRay(_cameraData, (float)x + 0.5f, (float)y + 0.5f);
			const glm::vec3 world = ray.origin + ray.direction * depth[i];
			const glm::vec4 previousClip = previousViewProjection * glm::vec4(world, 1.0f);
			float prevX = (float)x + 0.5f;
			float prevY = (float)y + 0.5f;
			if (previousClip.w > 1.0e-6f) {
				const float ndcX = previousClip.x / previousClip.w;
				const float ndcY = previousClip.y / previousClip.w;
				prevX = (ndcX * 0.5f + 0.5f) * (float)w;
				prevY = (1.0f - ndcY * 0.5f) * (float)h;
			}
			const int px = glm::clamp((int)glm::floor(prevX), 0, w - 1);
			const int py = glm::clamp((int)glm::floor(prevY), 0, h - 1);
			const int pi = py * w + px;
			const int pi3 = pi * 3;

			// History rejection: normal, depth, and albedo consistency (SVGF).
			const float ndot = glm::clamp(normal[i3 + 0] * _temporalNormal[pi3 + 0] +
										 normal[i3 + 1] * _temporalNormal[pi3 + 1] +
										 normal[i3 + 2] * _temporalNormal[pi3 + 2], 0.0f, 1.0f);
			const float normalWeight = glm::pow(ndot, 32.0f);
			const float depthScale = 0.03f * glm::max(depth[i], _temporalDepth[pi]) + 0.01f;
			const float depthDelta = glm::abs(depth[i] - _temporalDepth[pi]) / depthScale;
			const float depthWeight = glm::exp(-depthDelta * depthDelta);
			const float da0 = albedo[i3 + 0] - _temporalAlbedo[pi3 + 0];
			const float da1 = albedo[i3 + 1] - _temporalAlbedo[pi3 + 1];
			const float da2 = albedo[i3 + 2] - _temporalAlbedo[pi3 + 2];
			const float albedoWeight = glm::exp(-(da0 * da0 + da1 * da1 + da2 * da2) * 80.0f);
			const float rejectWeight = normalWeight * depthWeight * albedoWeight;

			const float historyCount = _temporalCount[pi];
			const float newCount = rejectWeight > 0.1f ? glm::min(historyCount + 1.0f, maxHistory) : 0.0f;
			const float temporalAlpha = 1.0f / (1.0f + newCount);
			const glm::vec3 historyColor(_temporalColor[pi3 + 0], _temporalColor[pi3 + 1],
										 _temporalColor[pi3 + 2]);
			const glm::vec3 spatialColor(rgb[i3 + 0], rgb[i3 + 1], rgb[i3 + 2]);
			const glm::vec3 blended = glm::mix(historyColor, spatialColor, temporalAlpha);

			nextColor[i3 + 0] = blended.x;
			nextColor[i3 + 1] = blended.y;
			nextColor[i3 + 2] = blended.z;
			nextNormal[i3 + 0] = normal[i3 + 0];
			nextNormal[i3 + 1] = normal[i3 + 1];
			nextNormal[i3 + 2] = normal[i3 + 2];
			nextAlbedo[i3 + 0] = albedo[i3 + 0];
			nextAlbedo[i3 + 1] = albedo[i3 + 1];
			nextAlbedo[i3 + 2] = albedo[i3 + 2];
			nextDepth[i] = depth[i];
			nextCount[i] = newCount;
		}
	}

	core_memcpy(_temporalColor.data(), nextColor.data(), (size_t)n * 3u * sizeof(float));
	core_memcpy(_temporalNormal.data(), nextNormal.data(), (size_t)n * 3u * sizeof(float));
	core_memcpy(_temporalAlbedo.data(), nextAlbedo.data(), (size_t)n * 3u * sizeof(float));
	core_memcpy(_temporalDepth.data(), nextDepth.data(), (size_t)n * sizeof(float));
	core_memcpy(_temporalCount.data(), nextCount.data(), (size_t)n * sizeof(float));
	_temporalCamera = _cameraData;

	// The temporally accumulated color replaces the spatial-only result.
	core_memcpy(rgb, _temporalColor.data(), (size_t)n * 3u * sizeof(float));
}

image::ImagePtr VoxelDDAPathTracer::image() {
#ifdef __EMSCRIPTEN__
	// The WebGPU backend denoises and tonemaps on the GPU; the cached RGBA8
	// image is returned directly instead of re-running the CPU post-process.
	if (_webGPUEnabled && !_webGPUImage.empty()) {
		image::ImagePtr img = image::createEmptyImage("pathtracer");
		if (img->loadRGBA(_webGPUImage.data(), _width, _height)) {
			return img;
		}
	}
#endif
	if (_width <= 0 || _height <= 0 || _accum.empty()) {
		return {};
	}
	const int n = _width * _height;
	core::Buffer<float> hdr;
	hdr.resize((size_t)n * 3u);
	const bool haveCounts = _accumCount.size() >= (size_t)n;
	for (int i = 0; i < n; ++i) {
		const float scale = haveCounts ? 1.0f / (float)glm::max(_accumCount[i], 1) : 1.0f;
		hdr[i * 3 + 0] = _accum[i * 4 + 0] * scale;
		hdr[i * 3 + 1] = _accum[i * 4 + 1] * scale;
		hdr[i * 3 + 2] = _accum[i * 4 + 2] * scale;
	}
	if (_state.params.denoise && _sample > 0) {
		denoiseColor(hdr.data());
	}
	core::Buffer<uint8_t> rgba;
	rgba.resize((size_t)n * 4u);
	for (int i = 0; i < n; ++i) {
		glm::vec3 c(hdr[i * 3 + 0], hdr[i * 3 + 1], hdr[i * 3 + 2]);
		const float scale = haveCounts ? 1.0f / (float)glm::max(_accumCount[i], 1) : 1.0f;
		const float a = glm::clamp(_accum[i * 4 + 3] * scale, 0.0f, 1.0f);
		const glm::vec3 ldr = pathTracerTonemap(c, _state.exposure, _state.filmic);
		rgba[i * 4 + 0] = (uint8_t)glm::clamp(ldr.x * 255.0f, 0.0f, 255.0f);
		rgba[i * 4 + 1] = (uint8_t)glm::clamp(ldr.y * 255.0f, 0.0f, 255.0f);
		rgba[i * 4 + 2] = (uint8_t)glm::clamp(ldr.z * 255.0f, 0.0f, 255.0f);
		rgba[i * 4 + 3] = (uint8_t)glm::clamp(a * 255.0f, 0.0f, 255.0f);
	}
	image::ImagePtr img = image::createEmptyImage("pathtracer");
	if (!img->loadRGBA(rgba.data(), _width, _height)) {
		return {};
	}
	return img;
}

IPathTracer *createPathTracer() {
	return new VoxelDDAPathTracer();
}

} // namespace voxelpathtracer
