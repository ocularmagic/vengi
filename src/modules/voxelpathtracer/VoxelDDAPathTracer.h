/**
 * @file
 */

#pragma once

#include "IPathTracer.h"
#include "PathTracerCamera.h"
#include "PathTracerMaterial.h"
#include "PathTracerScene.h"
#include "PathTracerState.h"
#ifdef __EMSCRIPTEN__
#include "PathTracerWebGPU.h"
#endif
#include "core/collection/Buffer.h"
#include "core/collection/DynamicArray.h"
#include "video/Camera.h"
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace voxelpathtracer {

class VoxelDDAPathTracer : public IPathTracer {
private:
	struct Hit {
		bool hit = false;
		bool ground = false;
		float t = 0.0f;
		glm::vec3 pos{0.0f};
		glm::vec3 normal{0.0f, 1.0f, 0.0f};
		glm::vec3 localPos{0.0f};
		glm::vec3 localNormal{0.0f, 1.0f, 0.0f};
		glm::vec3 albedo{0.0f};
		glm::vec3 emit{0.0f};
		int gridIndex = -1;
		glm::ivec3 cell{0};
		float opacity = 1.0f;
		float ior = 1.0f;
		float atten = 0.0f;
		float metal = 0.0f;
		float rough = 0.1f;
		float spec = 0.5f;
		float density = 0.0f;
		float phase = 0.0f;
		float media = 0.0f;
		uint8_t surf = 0;
	};

	PathTracerState _state;
	video::Camera _camera;
	PathTracerCameraData _cameraData;
	bool _hasCamera = false;
	PathTracerScene _scene;
	core::Buffer<float> _envRgba;
	core::Buffer<float> _envCdf;
	float _envCdfSum = 0.0f;
	int _envW = 0;
	int _envH = 0;
	bool _envIsHdri = false;
	core::Buffer<float> _accum;
	core::Buffer<float> _accumAlbedo;
	core::Buffer<float> _accumNormal;
	core::Buffer<float> _accumDepth;
	core::Buffer<float> _accumLuma2;
	core::Buffer<float> _accumFeature;
	int _width = 0;
	int _height = 0;
	int _sample = 0;
	bool _hasMedia = false;
#ifdef __EMSCRIPTEN__
	PathTracerWebGPU _webGPU;
	core::Buffer<PathTracerSampleOutput> _webGPUOutputs;
	bool _webGPUScenePending = false;
	bool _webGPUEnvironmentPending = false;
	bool _webGPUEnabled = false;
	bool _webGPUDispatchPending = false;
	PathTracerWebGPULifecycle _webGPULifecycle;
#endif

	bool loadHdri(const core::String &path);
	void buildEnvCdf();
	void buildGrids(const scenegraph::SceneGraph &sceneGraph);
	void buildEmitLights();
	glm::vec3 toWorld(const PathTracerGrid &grid, const glm::vec3 &local) const;
	bool visibleToLight(const glm::vec3 &orig, const glm::vec3 &target, bool fromGround, int skipGrid,
						const glm::ivec3 &skipCell, int lightGrid, const glm::ivec3 &lightCell) const;
	bool sampleEmitLight(const glm::vec3 &p, const glm::vec3 &n, uint32_t sequenceIndex, uint32_t scramble, int skipGrid,
						 const glm::ivec3 &skipCell, glm::vec3 &dir, glm::vec3 &radiance, float &pdf) const;
	float emitLightPdf(const glm::vec3 &p, const Hit &hit) const;
	glm::vec3 evalEnvironment(const glm::vec3 &dir) const;
	float environmentPdf(const glm::vec3 &dir) const;
	bool sampleHdri(uint32_t sequenceIndex, uint32_t scramble, glm::vec3 &dir, glm::vec3 &radiance,
					float &pdf) const;
	bool sampleEnvironment(const glm::vec3 &n, uint32_t sequenceIndex, uint32_t scramble, glm::vec3 &dir,
						   glm::vec3 &radiance, float &pdf) const;
	bool sampleEnvironmentIso(uint32_t sequenceIndex, uint32_t scramble, glm::vec3 &dir, glm::vec3 &radiance,
							  float &pdf) const;
	bool sampleMedia(const glm::vec3 &worldPos, glm::vec3 &albedo, float &density, float &phase, float &scatter,
					 glm::vec3 &emit) const;
	glm::vec3 fieldMediaT(const glm::vec3 &orig, const glm::vec3 &dir, float tmax) const;
	glm::vec3 marchVolume(const glm::vec3 &orig, const glm::vec3 &dir, float tmax, int skipGrid,
						  const glm::ivec3 &skipCell, glm::vec3 &color, const glm::vec3 &throughput,
						  uint32_t sequenceIndex, uint32_t scramble) const;
	bool pointInVoxel(const glm::vec3 &worldPos) const;
	bool occluded(const glm::vec3 &orig, const glm::vec3 &dir, bool fromGround, int skipGrid,
				  const glm::ivec3 &skipCell) const;
	glm::vec3 shadowTransmittance(const glm::vec3 &orig, const glm::vec3 &dir, float tmax, int skipGrid,
								  const glm::ivec3 &skipCell, int stopGrid, const glm::ivec3 &stopCell) const;
	bool traceGrid(const PathTracerGrid &grid, int gridIndex, const glm::vec3 &orig, const glm::vec3 &dir, float tmax,
				   int skipGrid, const glm::ivec3 &skipCell, bool shadowRay, Hit &hit) const;
	Hit trace(const glm::vec3 &orig, const glm::vec3 &dir, int skipGrid = -1,
			  const glm::ivec3 &skipCell = glm::ivec3(0), bool shadowRay = false) const;
	glm::vec4 tracePath(const glm::vec3 &orig, const glm::vec3 &dir, int sampleIndex,
						uint32_t pixelScramble, glm::vec3 &guideAlbedo, glm::vec3 &guideNormal, float &guideDepth,
						float &guideFeature) const;
	void accumulateSample();
	void denoiseColor(float *rgb) const;

public:
	VoxelDDAPathTracer();
	~VoxelDDAPathTracer() override;

	PathTracerState &state() override;
	const PathTracerState &state() const override;
	void applyAppearanceFromScene(const scenegraph::SceneGraph &sceneGraph) override;
	bool writeAppearanceToScene(const scenegraph::SceneGraph &sceneGraph) const override;
	bool start(const scenegraph::SceneGraph &sceneGraph, const video::Camera *camera = nullptr) override;
	bool restart(const scenegraph::SceneGraph &sceneGraph, const video::Camera *camera = nullptr) override;
	bool stop() override;
	bool started() const override;
	bool update(int *currentSample = nullptr) override;
	image::ImagePtr image() override;
};

} // namespace voxelpathtracer
