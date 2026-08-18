/**
 * @file
 */

#include "voxelpathtracer/IPathTracer.h"
#include "voxelpathtracer/Appearance.h"
#include "voxelpathtracer/PathTracer.h"
#include "voxelpathtracer/PathTracerCamera.h"
#include "voxelpathtracer/PathTracerMaterial.h"
#include "voxelpathtracer/PathTracerPrimary.h"
#include "voxelpathtracer/PathTracerScene.h"
#include "voxelpathtracer/PathTracerState.h"
#include "voxelpathtracer/PathTracerSampling.h"
#include "voxelpathtracer/PathTracerTonemap.h"
#include "voxelpathtracer/PathTracerTraversal.h"
#include "voxelpathtracer/PathTracerTraversalWGSL.h"
#include "voxelpathtracer/PathTracerWebGPU.h"
#include "voxelpathtracer/VoxelDDAPathTracer.h"
#include "voxelpathtracer/PathTracerHdri.h"
#include "tinyexr.h"
#include "app/App.h"
#include "app/tests/AbstractTest.h"
#include "color/RGBA.h"
#include "palette/Material.h"
#include "image/Image.h"
#include "io/File.h"
#include "io/FileStream.h"
#include "io/Filesystem.h"
#include "io/FilesystemArchive.h"
#include "io/FormatDescription.h"
#include "palette/Palette.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "scenegraph/SceneGraphNodeProperties.h"
#include "voxelformat/FormatConfig.h"
#include "voxelformat/VolumeFormat.h"
#include "core/ConfigVar.h"
#include "core/GLM.h"
#include "core/SharedPtr.h"
#include "core/String.h"
#include "core/StringUtil.h"
#include "core/Var.h"
#include "video/Camera.h"
#include "voxel/RawVolume.h"
#include "voxel/SurfaceExtractor.h"
#include "voxel/Voxel.h"

class PathTracerTest : public app::AbstractTest {
private:
	using Super = app::AbstractTest;

public:
	bool onInitApp() override {
		if (!Super::onInitApp()) {
			return false;
		}
		voxelformat::FormatConfig::init();
		const core::VarDef meshMode(cfg::VoxelMeshMode, (int)voxel::SurfaceExtractionType::Cubic, "", "");
		core::Var::registerVar(meshMode);
		const core::VarDef mergeQuads(cfg::VoxelMergeQuads, true, "", "");
		core::Var::registerVar(mergeQuads);
		return true;
	}
};

TEST_F(PathTracerTest, testPortableSamplingContract) {
	using namespace voxelpathtracer::sampling;
	EXPECT_EQ(663891101u, hash32(1u));
	EXPECT_EQ(4159036222u, hash32(0xa511e9b3u));
	uint32_t state = 1u;
	EXPECT_NEAR(0.154574156f, next1D(state), 0.0000001f);
	EXPECT_EQ(663891101u, state);

	// Owen-scrambled Sobol replaced the old R2 recurrence. Bit reversal and the
	// radical inverse are exact; the raw 2D Sobol is a perfect (0,2)-net.
	EXPECT_EQ(0u, reverseBits32(0u));
	EXPECT_EQ(0x80000000u, reverseBits32(1u));
	EXPECT_EQ(1u, reverseBits32(0x80000000u));
	EXPECT_EQ(0x1e6a2c48u, reverseBits32(0x12345678u));
	EXPECT_EQ(0u, sobolRaw(0u, 0u));
	EXPECT_EQ(0x80000000u, sobolRaw(1u, 0u));
	EXPECT_EQ(0xc0000000u, sobolRaw(2u, 1u));

	// The raw first 16 (dim0, dim1) points stratify every 4x4 cell exactly once.
	bool occupied[4][4] = {};
	for (uint32_t i = 0u; i < 16u; ++i) {
		const int cx = (int)(sobolRaw(i, 0u) >> 30u);
		const int cy = (int)(sobolRaw(i, 1u) >> 30u);
		EXPECT_FALSE(occupied[cy][cx]) << "duplicate Sobol cell at index " << i;
		occupied[cy][cx] = true;
	}
	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			EXPECT_TRUE(occupied[y][x]) << "Sobol missed cell (" << x << "," << y << ")";
		}
	}

	// The Owen-scrambled 2D sampler is deterministic and in [0,1).
	const glm::vec2 s0 = sobol2D(3u, 0xa511e9b3u);
	EXPECT_NEAR(s0.x, sobol2D(3u, 0xa511e9b3u).x, 0.0f);
	EXPECT_NEAR(s0.y, sobol2D(3u, 0xa511e9b3u).y, 0.0f);
	for (uint32_t i = 0u; i < 64u; ++i) {
		const glm::vec2 p = sobol2D(i, 7u);
		EXPECT_GE(p.x, 0.0f);
		EXPECT_LT(p.x, 1.0f);
		EXPECT_GE(p.y, 0.0f);
		EXPECT_LT(p.y, 1.0f);
	}

	// Golden values pin CPU<->WGSL bit-parity of the Owen-scrambled hash and
	// the full 1D/2D sample output (same algorithm, same constants).
	EXPECT_EQ(0x00000000u, sobolOwenHash(0u, 0u));
	EXPECT_EQ(0x64f83209u, sobolOwenHash(0x80000000u, 7u));
	EXPECT_FLOAT_EQ(0.934451044f, sobol1D(0u, 0u));
	EXPECT_FLOAT_EQ(0.434451044f, sobol1D(1u, 0u));
	EXPECT_FLOAT_EQ(0.684451044f, sobol1D(2u, 0u));
	EXPECT_FLOAT_EQ(0.184451044f, sobol1D(3u, 0u));
	const glm::vec2 g0 = sobol2D(0u, 0u);
	EXPECT_FLOAT_EQ(0.108125567f, g0.x);
	EXPECT_FLOAT_EQ(0.289022863f, g0.y);
	const glm::vec2 g3 = sobol2D(3u, 0xa511e9b3u);
	EXPECT_FLOAT_EQ(0.044486463f, g3.x);
	EXPECT_FLOAT_EQ(0.760713518f, g3.y);
}

TEST_F(PathTracerTest, testHdriPathFallsBackToPortableBasename) {
	const core::String filename = "portable_hdri_reference.hdr";
	ASSERT_TRUE(_testApp->filesystem()->homeWrite(filename, "hdr"));
	const core::String resolved =
		voxelpathtracer::resolveHdriPath(core::string::path("C:/unavailable/desktop/assets", filename));
	EXPECT_FALSE(resolved.empty());
	EXPECT_EQ(core::string::extractFilenameWithExtension(resolved), filename);
	ASSERT_TRUE(io::Filesystem::sysRemoveFile(_testApp->filesystem()->homeWritePath(filename)));
}

TEST_F(PathTracerTest, testPortableCameraRayMatchesEditorCamera) {
	EXPECT_EQ(80u, sizeof(voxelpathtracer::PathTracerCameraData));
	EXPECT_EQ(16u, alignof(voxelpathtracer::PathTracerCameraData));
	EXPECT_EQ(64u, offsetof(voxelpathtracer::PathTracerCameraData, viewport));
	EXPECT_EQ(32u, sizeof(voxelpathtracer::PathTracerPrimaryParams));
	EXPECT_EQ(16u, alignof(voxelpathtracer::PathTracerPrimaryParams));
	video::Camera camera;
	camera.setSize(glm::ivec2(80, 40));
	camera.setWorldPosition(glm::vec3(4.0f, 3.0f, 7.0f));
	camera.lookAt(glm::vec3(0.5f, 0.25f, -0.5f));
	camera.update(0.0);
	const glm::ivec2 pixel(17, 9);
	const math::Ray expected = camera.mouseRay(pixel);
	const voxelpathtracer::PathTracerCameraData data = voxelpathtracer::pathTracerCameraData(camera);
	const math::Ray actual = voxelpathtracer::pathTracerCameraRay(data, (float)pixel.x, (float)pixel.y);
	EXPECT_NEAR(expected.origin.x, actual.origin.x, 0.0001f);
	EXPECT_NEAR(expected.origin.y, actual.origin.y, 0.0001f);
	EXPECT_NEAR(expected.origin.z, actual.origin.z, 0.0001f);
	EXPECT_NEAR(expected.direction.x, actual.direction.x, 0.0001f);
	EXPECT_NEAR(expected.direction.y, actual.direction.y, 0.0001f);
	EXPECT_NEAR(expected.direction.z, actual.direction.z, 0.0001f);

	voxelpathtracer::PathTracerPrimaryParams primaryParams;
	primaryParams.pixelCount = 80u * 40u;
	primaryParams.sampleIndex = 3u;
	const uint32_t pixelIndex = static_cast<uint32_t>(pixel.y) * 80u + static_cast<uint32_t>(pixel.x);
	const uint32_t scramble = voxelpathtracer::sampling::hash32(pixelIndex ^ 0xa511e9b3u);
	const glm::vec2 jitter = voxelpathtracer::sampling::sobol2D(primaryParams.sampleIndex, scramble);
	const math::Ray expectedPrimary = voxelpathtracer::pathTracerCameraRay(
		data, static_cast<float>(pixel.x) + jitter.x, static_cast<float>(pixel.y) + jitter.y);
	const voxelpathtracer::PathTracerRay primary =
		voxelpathtracer::pathTracerPrimaryRay(data, primaryParams, pixelIndex);
	EXPECT_NEAR(expectedPrimary.origin.x, primary.origin().x, 0.0001f);
	EXPECT_NEAR(expectedPrimary.origin.y, primary.origin().y, 0.0001f);
	EXPECT_NEAR(expectedPrimary.origin.z, primary.origin().z, 0.0001f);
	EXPECT_NEAR(expectedPrimary.direction.x, primary.direction().x, 0.0001f);
	EXPECT_NEAR(expectedPrimary.direction.y, primary.direction().y, 0.0001f);
	EXPECT_NEAR(expectedPrimary.direction.z, primary.direction().z, 0.0001f);

	camera.setMode(video::CameraMode::Orthogonal);
	camera.update(0.0);
	const math::Ray expectedOrtho = camera.mouseRay(pixel);
	const voxelpathtracer::PathTracerCameraData orthoData = voxelpathtracer::pathTracerCameraData(camera);
	const math::Ray actualOrtho = voxelpathtracer::pathTracerCameraRay(orthoData, (float)pixel.x, (float)pixel.y);
	EXPECT_NEAR(expectedOrtho.direction.x, actualOrtho.direction.x, 0.0001f);
	EXPECT_NEAR(expectedOrtho.direction.y, actualOrtho.direction.y, 0.0001f);
	EXPECT_NEAR(expectedOrtho.direction.z, actualOrtho.direction.z, 0.0001f);
	const glm::vec3 originDelta = actualOrtho.origin - expectedOrtho.origin;
	EXPECT_LT(glm::length(glm::cross(originDelta, expectedOrtho.direction)), 0.001f);
}

TEST_F(PathTracerTest, testPortableMaterialLayout) {
	voxelpathtracer::PathTracerMaterial material;
	EXPECT_EQ(96u, sizeof(material));
	EXPECT_EQ(16u, alignof(voxelpathtracer::PathTracerMaterial));
	material.albedoOpacity = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
	material.emissionIor = glm::vec4(1.0f, 2.0f, 3.0f, 1.5f);
	material.volumeEmissionAttenuation = glm::vec4(4.0f, 5.0f, 6.0f, 0.7f);
	material.surface = glm::vec4(0.8f, 0.6f, 0.5f, 0.3f);
	material.volume = glm::vec4(0.25f, 0.75f, 0.0f, 0.0f);
	material.flags = glm::uvec4(3u, 0u, 0u, 0u);
	EXPECT_EQ(glm::vec3(0.1f, 0.2f, 0.3f), material.albedo());
	EXPECT_FLOAT_EQ(0.4f, material.opacity());
	EXPECT_EQ(glm::vec3(1.0f, 2.0f, 3.0f), material.emission());
	EXPECT_FLOAT_EQ(1.5f, material.ior());
	EXPECT_EQ(glm::vec3(4.0f, 5.0f, 6.0f), material.volumeEmission());
	EXPECT_FLOAT_EQ(0.7f, material.attenuation());
	EXPECT_FLOAT_EQ(0.8f, material.metal());
	EXPECT_FLOAT_EQ(0.6f, material.roughness());
	EXPECT_FLOAT_EQ(0.5f, material.specular());
	EXPECT_FLOAT_EQ(0.3f, material.density());
	EXPECT_FLOAT_EQ(0.25f, material.phase());
	EXPECT_FLOAT_EQ(0.75f, material.media());
	EXPECT_EQ(3u, material.surfaceType());
}

TEST_F(PathTracerTest, testPortableSceneLayoutAndIndexing) {
	using namespace voxelpathtracer;
	EXPECT_EQ(192u, sizeof(PathTracerGrid));
	EXPECT_EQ(16u, alignof(PathTracerGrid));
	EXPECT_EQ(96u, sizeof(PathTracerEmitter));
	EXPECT_EQ(16u, alignof(PathTracerEmitter));
	EXPECT_EQ(80u, offsetof(PathTracerEmitter, cellGrid));
	EXPECT_EQ(48u, sizeof(PathTracerGround));
	EXPECT_EQ(16u, alignof(PathTracerGround));

	PathTracerScene scene;
	const glm::ivec3 mins(-2, 4, 7);
	const glm::ivec3 size(2, 3, 4);
	PathTracerGrid &first = scene.addGrid(mins, size, glm::mat4(1.0f), glm::mat4(1.0f), glm::vec3(0.5f));
	EXPECT_EQ(mins, first.mins());
	EXPECT_EQ(size, first.size());
	EXPECT_EQ(0u, first.cellOffset());
	EXPECT_EQ(0u, first.materialOffset());
	EXPECT_EQ(24u, first.cellCount());
	EXPECT_EQ(24u, scene.cells.size());
	EXPECT_EQ(256u, scene.materials.size());
	EXPECT_TRUE(first.contains(mins));
	EXPECT_TRUE(first.contains(mins + size - 1));
	EXPECT_FALSE(first.contains(mins - glm::ivec3(1, 0, 0)));
	EXPECT_FALSE(first.contains(mins + size));
	EXPECT_EQ(0u, first.localCellIndex(mins));
	EXPECT_EQ(23u, first.localCellIndex(mins + size - 1));

	scene.cell(first, first.localCellIndex(glm::ivec3(-1, 6, 9))) = 42u;
	EXPECT_EQ(42u, scene.cell(first, first.localCellIndex(glm::ivec3(-1, 6, 9))));
	scene.material(first, 17u).flags.x = 3u;
	EXPECT_EQ(3u, scene.material(first, 17u).surfaceType());

	const uint32_t firstCellOffset = first.cellOffset();
	const uint32_t firstMaterialOffset = first.materialOffset();
	PathTracerGrid &second = scene.addGrid(glm::ivec3(10), glm::ivec3(1), glm::mat4(1.0f), glm::mat4(1.0f),
											 glm::vec3(0.0f));
	EXPECT_EQ(24u, second.cellOffset());
	EXPECT_EQ(256u, second.materialOffset());
	EXPECT_EQ(25u, scene.cells.size());
	EXPECT_EQ(512u, scene.materials.size());
	EXPECT_EQ(42u, scene.cells[firstCellOffset + 17u]);
	EXPECT_EQ(3u, scene.materials[firstMaterialOffset + 17u].surfaceType());
	EXPECT_EQ(0u, scene.cell(second, 0u));
}

TEST_F(PathTracerTest, testPortableTraversalContract) {
	using namespace voxelpathtracer;
	EXPECT_EQ(64u, sizeof(PathTracerRay));
	EXPECT_EQ(96u, sizeof(PathTracerVoxelHit));
	EXPECT_EQ(16u, sizeof(PathTracerDispatchParams));
	EXPECT_EQ(64u, sizeof(PathTracerLightingData));
	EXPECT_EQ(32u, sizeof(PathTracerEnvironmentData));
	EXPECT_EQ(32u, sizeof(PathTracerMediaData));
	EXPECT_EQ(96u, sizeof(PathTracerSampleOutput));
	EXPECT_EQ(16u, alignof(PathTracerRay));
	EXPECT_EQ(16u, alignof(PathTracerVoxelHit));
	EXPECT_EQ(16u, alignof(PathTracerDispatchParams));
	EXPECT_EQ(16u, alignof(PathTracerLightingData));
	EXPECT_EQ(16u, alignof(PathTracerEnvironmentData));
	EXPECT_EQ(16u, alignof(PathTracerMediaData));
	EXPECT_EQ(16u, alignof(PathTracerSampleOutput));
	EXPECT_EQ(80u, offsetof(PathTracerSampleOutput, moments));
	EXPECT_EQ(16u, offsetof(PathTracerRay, directionMax));
	EXPECT_EQ(32u, offsetof(PathTracerRay, skipCellGrid));
	EXPECT_EQ(48u, offsetof(PathTracerRay, flags));
	EXPECT_EQ(16u, offsetof(PathTracerVoxelHit, normal));
	EXPECT_EQ(32u, offsetof(PathTracerVoxelHit, localPosition));
	EXPECT_EQ(48u, offsetof(PathTracerVoxelHit, localNormal));
	EXPECT_EQ(64u, offsetof(PathTracerVoxelHit, cellGrid));
	EXPECT_EQ(80u, offsetof(PathTracerVoxelHit, data));

	PathTracerScene scene;
	PathTracerGrid &grid = scene.addGrid(glm::ivec3(0), glm::ivec3(1, 1, 2), glm::mat4(1.0f),
										glm::mat4(1.0f), glm::vec3(0.0f));
	scene.material(grid, 0u).flags.x = PathTracerSurfaceOpaque;
	scene.material(grid, 1u).flags.x = PathTracerSurfaceGlass;
	scene.cell(grid, grid.localCellIndex(glm::ivec3(0, 0, 0))) = 1u;
	scene.cell(grid, grid.localCellIndex(glm::ivec3(0, 0, 1))) = 2u;

	PathTracerVoxelHit hit = pathTracerTraceScene(
		scene, pathTracerRay(glm::vec3(0.5f, 0.5f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f), 100.0f));
	ASSERT_TRUE(hit.hit());
	EXPECT_EQ(glm::ivec3(0, 0, 1), hit.cell());
	EXPECT_EQ(1u, hit.materialIndex());
	EXPECT_NEAR(1.0f, hit.positionT.w, 0.0001f);
	EXPECT_EQ(glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(hit.normal));

	const PathTracerVoxelHit shadowHit = pathTracerTraceScene(
		scene, pathTracerRay(glm::vec3(0.5f, 0.5f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f), 100.0f, -1,
						  glm::ivec3(0), true));
	ASSERT_TRUE(shadowHit.hit());
	EXPECT_EQ(glm::ivec3(0, 0, 0), shadowHit.cell());
	EXPECT_EQ(0u, shadowHit.materialIndex());
	EXPECT_NEAR(2.0f, shadowHit.positionT.w, 0.0001f);

	const PathTracerVoxelHit skipped = pathTracerTraceScene(
		scene, pathTracerRay(glm::vec3(0.5f), glm::vec3(0.0f, 0.0f, 1.0f), 100.0f, 0,
						  glm::ivec3(0, 0, 0), false));
	ASSERT_TRUE(skipped.hit());
	EXPECT_EQ(glm::ivec3(0, 0, 1), skipped.cell());
	EXPECT_NEAR(0.5f, skipped.positionT.w, 0.0001f);

	PathTracerRay rays[3] = {
		pathTracerRay(glm::vec3(0.5f, 0.5f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f), 100.0f),
		pathTracerRay(glm::vec3(0.5f, 0.5f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f), 100.0f, -1,
					  glm::ivec3(0), true),
		pathTracerRay(glm::vec3(2.0f, 2.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f), 100.0f)};
	PathTracerVoxelHit hits[3];
	PathTracerDispatchParams dispatch;
	dispatch.rayCount = 3u;
	dispatch.gridCount = 1u;
	ASSERT_TRUE(pathTracerTraceRays(scene, rays, hits, dispatch));
	EXPECT_EQ(glm::ivec3(0, 0, 1), hits[0].cell());
	EXPECT_EQ(glm::ivec3(0, 0, 0), hits[1].cell());
	EXPECT_FALSE(hits[2].hit());
	dispatch.gridCount = 2u;
	EXPECT_FALSE(pathTracerTraceRays(scene, rays, hits, dispatch));
	dispatch.gridCount = 1u;
	EXPECT_FALSE(pathTracerTraceRays(scene, nullptr, hits, dispatch));

	PathTracerScene transformedScene;
	const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 2.0f, 1.0f)) *
							glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 0.5f));
	PathTracerGrid &transformed = transformedScene.addGrid(glm::ivec3(0), glm::ivec3(1), world,
													 glm::inverse(world), glm::vec3(0.0f));
	transformedScene.material(transformed, 0u).flags.x = PathTracerSurfaceOpaque;
	transformedScene.cell(transformed, 0u) = 1u;
	hit = pathTracerTraceScene(
		transformedScene, pathTracerRay(glm::vec3(4.0f, 2.5f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f), 100.0f));
	ASSERT_TRUE(hit.hit());
	EXPECT_NEAR(1.5f, hit.positionT.w, 0.0001f);
	EXPECT_NEAR(1.5f, hit.positionT.z, 0.0001f);
	EXPECT_EQ(glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(hit.normal));
}

TEST_F(PathTracerTest, testPortableWebGPUAccumulationCopy) {
	using namespace voxelpathtracer;
	PathTracerSampleOutput outputs[2];
	for (int i = 0; i < 2; ++i) {
		outputs[i].radianceAlpha = glm::vec4(1.0f + (float)i, 2.0f, 3.0f, 4.0f);
		outputs[i].albedoFeature = glm::vec4(0.2f, 0.3f, 0.4f, 0.7f);
		outputs[i].normalDepth = glm::vec4(0.0f, 1.0f, 0.0f, 5.0f + (float)i);
		outputs[i].moments = glm::vec4(6.0f + (float)i, 4.0f, 0.0f, 0.0f);
	}
	float rgba[8] = {0.0f};
	float albedo[6] = {0.0f};
	float normal[6] = {0.0f};
	float depth[2] = {0.0f};
	float luminanceSquared[2] = {0.0f};
	float feature[2] = {0.0f};
	int sampleCounts[2] = {0};
	EXPECT_EQ(4u, pathTracerCopySampleOutputs(outputs, 2u, rgba, albedo, normal, depth, luminanceSquared,
		feature, sampleCounts));
	EXPECT_FLOAT_EQ(1.0f, rgba[0]);
	EXPECT_FLOAT_EQ(2.0f, rgba[4]);
	EXPECT_FLOAT_EQ(0.3f, albedo[1]);
	EXPECT_FLOAT_EQ(1.0f, normal[1]);
	EXPECT_FLOAT_EQ(6.0f, depth[1]);
	EXPECT_FLOAT_EQ(7.0f, luminanceSquared[1]);
	EXPECT_FLOAT_EQ(0.7f, feature[1]);
	EXPECT_EQ(4, sampleCounts[0]);
	EXPECT_EQ(4, sampleCounts[1]);
	// Adaptive sampling allows per-pixel counts to differ; the maximum is the
	// global pass count and each pixel's own count is preserved.
	outputs[1].moments.y = 3.0f;
	EXPECT_EQ(4u, pathTracerCopySampleOutputs(outputs, 2u, rgba, albedo, normal, depth, luminanceSquared,
		feature, sampleCounts));
	EXPECT_EQ(4, sampleCounts[0]);
	EXPECT_EQ(3, sampleCounts[1]);
	// A zero count and a non-finite count are still rejected as invalid.
	outputs[0].moments.y = 0.0f;
	outputs[1].moments.y = 0.0f;
	EXPECT_EQ(0u, pathTracerCopySampleOutputs(outputs, 2u, rgba, albedo, normal, depth, luminanceSquared,
		feature, sampleCounts));
	outputs[0].moments.y = 4.0f;
	outputs[1].moments.y = glm::sqrt(-1.0f); // NaN
	EXPECT_EQ(0u, pathTracerCopySampleOutputs(outputs, 2u, rgba, albedo, normal, depth, luminanceSquared,
		feature, sampleCounts));
}

TEST_F(PathTracerTest, testWGSLTraversalABIContract) {
	const core::String source(voxelpathtracer::pathTracerTraversalWGSL());
	EXPECT_TRUE(source.contains("struct PathTracerGrid"));
	EXPECT_TRUE(source.contains("minsData: vec4<i32>"));
	EXPECT_TRUE(source.contains("worldMat: mat4x4<f32>"));
	EXPECT_TRUE(source.contains("struct PathTracerMaterial"));
	EXPECT_TRUE(source.contains("flags: vec4<u32>"));
	EXPECT_TRUE(source.contains("struct PathTracerRay"));
	EXPECT_TRUE(source.contains("skipCellGrid: vec4<i32>"));
	EXPECT_TRUE(source.contains("struct PathTracerVoxelHit"));
	EXPECT_TRUE(source.contains("cellGrid: vec4<i32>"));
	EXPECT_TRUE(source.contains("struct PathTracerDispatchParams"));
	EXPECT_TRUE(source.contains("struct PathTracerCameraData"));
	EXPECT_TRUE(source.contains("struct PathTracerPrimaryParams"));
	EXPECT_TRUE(source.contains("struct PathTracerLightingData"));
	EXPECT_TRUE(source.contains("struct PathTracerSampleOutput"));
	EXPECT_TRUE(source.contains("struct PathTracerEmitter"));
	EXPECT_TRUE(source.contains("struct PathTracerGround"));
	EXPECT_TRUE(source.contains("struct PathTracerEnvironmentData"));
	EXPECT_TRUE(source.contains("struct PathTracerMediaData"));
	EXPECT_TRUE(source.contains("@group(0) @binding(0) var<storage, read> grids"));
	EXPECT_TRUE(source.contains("@group(0) @binding(1) var<storage, read> cells"));
	EXPECT_TRUE(source.contains("@group(0) @binding(2) var<storage, read> materials"));
	EXPECT_TRUE(source.contains("@group(0) @binding(3) var<storage, read> rays"));
	EXPECT_TRUE(source.contains("@group(0) @binding(4) var<storage, read_write> hits"));
	EXPECT_TRUE(source.contains("@compute @workgroup_size(64)"));
	EXPECT_TRUE(source.contains("fn primaryMain"));
	EXPECT_TRUE(source.contains("@group(0) @binding(6) var<uniform> cameraData"));
	EXPECT_TRUE(source.contains("@group(0) @binding(7) var<uniform> primaryParams"));
	EXPECT_TRUE(source.contains("@group(0) @binding(8) var<uniform> lightingData"));
	EXPECT_TRUE(source.contains("@group(0) @binding(9) var<storage, read_write> sampleOutputs"));
	EXPECT_TRUE(source.contains("@group(0) @binding(10) var<storage, read> emitters"));
	EXPECT_TRUE(source.contains("@group(0) @binding(11) var<uniform> groundData"));
	EXPECT_TRUE(source.contains("@group(0) @binding(12) var<storage, read> environmentTexels"));
	EXPECT_TRUE(source.contains("@group(0) @binding(13) var<storage, read> environmentCdf"));
	EXPECT_TRUE(source.contains("@group(0) @binding(14) var<uniform> environmentData"));
	EXPECT_TRUE(source.contains("@group(0) @binding(15) var<uniform> mediaData"));
	EXPECT_TRUE(source.contains("fn shadePrimary"));
	EXPECT_TRUE(source.contains("fn sampleEmitter"));
	EXPECT_TRUE(source.contains("fn emitterHitPdf"));
	EXPECT_TRUE(source.contains("fn traceGround"));
	EXPECT_TRUE(source.contains("fn traceTransportScene"));
	EXPECT_TRUE(source.contains("fn evalHdri"));
	EXPECT_TRUE(source.contains("fn environmentPdf"));
	EXPECT_TRUE(source.contains("fn sampleEnvironment"));
	EXPECT_TRUE(source.contains("fn mediaAt"));
	EXPECT_TRUE(source.contains("fn integrateMedia"));
	EXPECT_TRUE(source.contains("fn henyeyGreenstein"));
	EXPECT_TRUE(source.contains("fn sobol2D"));
	EXPECT_TRUE(source.contains("fn sobol1D"));
	EXPECT_TRUE(source.contains("fn sobolOwenHash"));
	EXPECT_TRUE(source.contains("reverseBits(index)"));
	EXPECT_TRUE(source.contains("countTrailingZeros(n)"));
	EXPECT_TRUE(source.contains("fn clampSampleHistory"));
	EXPECT_TRUE(source.contains("fn sampleGgxVisibleHalf"));
	EXPECT_TRUE(source.contains("fn opaquePdf"));
	EXPECT_TRUE(source.contains("fn beerAttenuation"));
	EXPECT_TRUE(source.contains("surfaceType == surfaceGlass"));
	EXPECT_TRUE(source.contains("surfaceType == surfaceAlpha"));
	EXPECT_TRUE(source.contains("let maxBounces = clamp(lightingData.flags.w, 1u, 8u)"));
	// Item 2: environment next-event estimation uses a single sample with full
	// MIS (1 NEE + 1 BSDF); the old four-sample stratification heuristic is gone.
	EXPECT_TRUE(source.contains("let environmentSample = sampleEnvironment(facingNormal, sequenceIndex"));
	EXPECT_TRUE(source.contains("includeEnvironmentMis = bounce + 1u < maxBounces"));
	EXPECT_TRUE(source.contains("powerHeuristic(lightPdf, bsdfPdf)"));
	EXPECT_TRUE(source.contains("previousEnvironmentPdf = environmentPdf(incoming);"));
	// Item 4: unbiased Russian roulette replaces the hard throughput cutoff.
	EXPECT_TRUE(source.contains("let survival = max(throughputMax, 1.0e-4)"));
	// Item 3: adaptive sampling convergence predicate and per-pixel early stop.
	EXPECT_TRUE(source.contains("adaptiveEnabled: u32"));
	EXPECT_TRUE(source.contains("adaptiveError: f32"));
	EXPECT_TRUE(source.contains("adaptiveMinSamples: u32"));
	EXPECT_TRUE(source.contains("fn pixelConverged"));
	EXPECT_TRUE(source.contains("primaryParams.adaptiveEnabled != 0u"));
}

TEST_F(PathTracerTest, testWebGPUBackendLifecycleContract) {
	using namespace voxelpathtracer;
	PathTracerWebGPU backend;
	EXPECT_EQ(PathTracerWebGPUState::Unavailable, backend.state());
	EXPECT_FALSE(backend.ready());
	EXPECT_FALSE(backend.busy());
#ifndef __EMSCRIPTEN__
	EXPECT_FALSE(backend.init());
	backend.update();
	EXPECT_EQ(PathTracerWebGPUState::Unavailable, backend.state());
	PathTracerScene scene;
	EXPECT_FALSE(backend.uploadScene(scene));
	float environmentTexel[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float environmentCdf[1] = {1.0f};
	EXPECT_FALSE(backend.uploadEnvironment(environmentTexel, environmentCdf, 1u, 1u, 1.0f));
	PathTracerRay ray;
	EXPECT_FALSE(backend.dispatch(&ray, 1u));
	PathTracerCameraData camera;
	PathTracerPrimaryParams primary;
	PathTracerLightingData lighting;
	EXPECT_EQ(1u, lighting.flags.w);
	primary.pixelCount = 1u;
	EXPECT_FALSE(backend.dispatchPrimary(camera, primary, lighting));
	PathTracerVoxelHit hit;
	PathTracerSampleOutput output;
	uint32_t hitCount = 1u;
	EXPECT_FALSE(backend.takeResults(&hit, 1u, hitCount));
	EXPECT_EQ(0u, hitCount);
	uint32_t pixelCount = 1u;
	EXPECT_FALSE(backend.takePrimaryResults(&hit, &output, 1u, pixelCount));
	EXPECT_EQ(0u, pixelCount);
	pixelCount = 1u;
	EXPECT_FALSE(backend.takePrimaryOutputs(&output, 1u, pixelCount));
	EXPECT_EQ(0u, pixelCount);
#endif
	backend.shutdown();
	EXPECT_EQ(PathTracerWebGPUState::Unavailable, backend.state());
	backend.abort();
	EXPECT_EQ(PathTracerWebGPUState::Unavailable, backend.state());
	EXPECT_FALSE(backend.ready());
#ifndef __EMSCRIPTEN__
	EXPECT_FALSE(backend.init());
	backend.abort();
	EXPECT_EQ(PathTracerWebGPUState::Unavailable, backend.state());
	EXPECT_FALSE(backend.uploadScene(scene));
	EXPECT_FALSE(backend.dispatchPrimary(camera, primary, lighting, 64u, false));
#endif
	backend.shutdown();
	EXPECT_EQ(PathTracerWebGPUState::Unavailable, backend.state());
	EXPECT_FALSE(backend.needsUpload());
	EXPECT_TRUE(backend.fallbackMessage().empty());
}

TEST_F(PathTracerTest, testWebGPULifecyclePolicy) {
	using namespace voxelpathtracer;
	PathTracerWebGPULifecycle life;
	EXPECT_TRUE(pathTracerWebGPUApplyEvent(life, PathTracerWebGPUEvent::DeviceLost));
	EXPECT_TRUE(life.enabled);
	EXPECT_TRUE(life.resetAccumulation);
	EXPECT_TRUE(life.needsUpload);
	EXPECT_EQ(1u, life.recoveries);
	EXPECT_STREQ(pathTracerWebGPUEventMessage(PathTracerWebGPUEvent::DeviceLost, true),
		life.message.c_str());
	EXPECT_FALSE(core::string::contains(life.message, "CPU"));

	EXPECT_FALSE(pathTracerWebGPUApplyEvent(life, PathTracerWebGPUEvent::DeviceLost));
	EXPECT_FALSE(life.enabled);
	EXPECT_FALSE(life.resetAccumulation);
	EXPECT_STREQ(pathTracerWebGPUEventMessage(PathTracerWebGPUEvent::DeviceLost, false),
		life.message.c_str());
	EXPECT_TRUE(core::string::contains(life.message, "continuing on the CPU renderer"));

	PathTracerWebGPULifecycle recovered;
	EXPECT_TRUE(pathTracerWebGPUApplyEvent(recovered, PathTracerWebGPUEvent::Recovered));
	EXPECT_TRUE(recovered.enabled);
	EXPECT_TRUE(recovered.resetAccumulation);
	EXPECT_EQ(1u, recovered.recoveries);
	EXPECT_STREQ("WebGPU device was lost; recovered and restarted the render", recovered.message.c_str());

	PathTracerWebGPULifecycle failed;
	EXPECT_FALSE(pathTracerWebGPUApplyEvent(failed, PathTracerWebGPUEvent::RecoverFailed));
	EXPECT_FALSE(failed.enabled);
	EXPECT_STREQ("WebGPU device was lost; continuing on the CPU renderer", failed.message.c_str());

	PathTracerWebGPULifecycle exhausted;
	EXPECT_FALSE(pathTracerWebGPUApplyEvent(exhausted, PathTracerWebGPUEvent::ResourceExhausted));
	EXPECT_STREQ("WebGPU ran out of GPU memory; continuing on the CPU renderer", exhausted.message.c_str());

	PathTracerWebGPULifecycle invalid;
	EXPECT_TRUE(pathTracerWebGPUApplyEvent(invalid, PathTracerWebGPUEvent::InvalidAccumulation));
	EXPECT_TRUE(invalid.enabled);
	EXPECT_TRUE(invalid.invalidRetry);
	EXPECT_STREQ("Retrying the first WebGPU batch", invalid.message.c_str());
	EXPECT_FALSE(pathTracerWebGPUApplyEvent(invalid, PathTracerWebGPUEvent::InvalidAccumulation));
	EXPECT_FALSE(invalid.enabled);
	EXPECT_TRUE(core::string::contains(invalid.message, "continuing on the CPU renderer"));

	PathTracerState state;
	EXPECT_TRUE(state.backendMessage.empty());
	state.backendMessage = failed.message;
	EXPECT_FALSE(state.backendMessage.empty());
}

TEST_F(PathTracerTest, testHMec) {
	const io::ArchivePtr &archive = io::openFilesystemArchive(_testApp->filesystem());
	io::FileDescription fileDesc;
	fileDesc.set("hmec.vxl");
	scenegraph::SceneGraph sceneGraph;
	voxelformat::LoadContext testLoadCtx;
	ASSERT_TRUE(voxelformat::loadFormat(fileDesc, archive, sceneGraph, testLoadCtx))
		<< "Could not load " << fileDesc.name.c_str();

	voxelpathtracer::PathTracer pathTracer;
	pathTracer.state().params.resolution = 512;
	pathTracer.state().params.samples = 8;
	ASSERT_TRUE(pathTracer.start(sceneGraph));
	while (!pathTracer.update()) {
		_testApp->wait(100);
	}
	const image::ImagePtr &img = pathTracer.image();
	ASSERT_TRUE(img);
	ASSERT_TRUE(img->isLoaded());
	ASSERT_EQ(512, img->width());
	// ASSERT_EQ(dimensions, img->height());
	const io::FilePtr &file = _testApp->filesystem()->open("hmec.vxl.png", io::FileMode::SysWrite);
	io::FileStream stream(file);
	ASSERT_TRUE(image::writePNG(img, stream));
	ASSERT_TRUE(pathTracer.stop());
}

TEST_F(PathTracerTest, testViewportCameraIsPrimary) {
	scenegraph::SceneGraph sceneGraph;
	video::Camera cam;
	cam.setSize(glm::ivec2(800, 600));
	cam.setRotationType(video::CameraRotationType::Target);
	cam.setWorldPosition(glm::vec3(10.0f, 20.0f, 30.0f));
	cam.setTarget(glm::vec3(0.0f, 0.0f, 0.0f));
	cam.lookAt(glm::vec3(0.0f, 0.0f, 0.0f));
	cam.setFieldOfView(45.0f);
	cam.update(0.0);
	const glm::vec3 eye = cam.worldPosition();

	voxelpathtracer::PathTracer pathTracer;
	ASSERT_TRUE(pathTracer.start(sceneGraph, &cam));

	const yocto::scene_data &scene = pathTracer.state().scene;
	ASSERT_FALSE(scene.cameras.empty());
	EXPECT_EQ(scene.camera_names[0], "viewport");
	EXPECT_EQ(pathTracer.state().params.camera, 0);
	// A live viewport camera must not get Yocto's extra framed fallback.
	EXPECT_EQ((int)scene.cameras.size(), 1);
	EXPECT_NEAR(scene.cameras[0].frame.o.x, eye.x, 0.01f);
	EXPECT_NEAR(scene.cameras[0].frame.o.y, eye.y, 0.01f);
	EXPECT_NEAR(scene.cameras[0].frame.o.z, eye.z, 0.01f);

	const float fovRadians = glm::radians(45.0f);
	const float aspect = 800.0f / 600.0f;
	float expectedDistance = 0.036f / (2.0f * glm::tan(fovRadians / 2.0f));
	expectedDistance /= aspect;
	const float focus = cam.targetDistance();
	const float expectedLens = focus * expectedDistance / (focus + expectedDistance);
	EXPECT_NEAR(scene.cameras[0].lens, expectedLens, 0.001f);

	ASSERT_TRUE(pathTracer.stop());
}

TEST_F(PathTracerTest, testStudioEnvironmentDefault) {
	scenegraph::SceneGraph sceneGraph;
	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(10.0f, 10.0f, 10.0f));
	cam.lookAt(glm::vec3(0.0f, 0.0f, 0.0f));
	cam.update(0.0);

	voxelpathtracer::PathTracer pathTracer;
	ASSERT_FALSE(pathTracer.state().skyEnvironment);
	ASSERT_NEAR(pathTracer.state().environmentColor.x, 0.91f, 0.001f);
	ASSERT_NEAR(pathTracer.state().environmentColor.y, 0.91f, 0.001f);
	ASSERT_NEAR(pathTracer.state().environmentColor.z, 0.92f, 0.001f);
	ASSERT_TRUE(pathTracer.start(sceneGraph, &cam));

	const yocto::scene_data &scene = pathTracer.state().scene;
	ASSERT_FALSE(scene.environments.empty());
	ASSERT_FALSE(scene.textures.empty());
	// Studio wrap is a small lat-long map, not the 1024x512 sunsky.
	EXPECT_EQ(scene.textures[0].width, 256);
	EXPECT_EQ(scene.textures[0].height, 128);

	ASSERT_TRUE(pathTracer.stop());
}

static video::Camera testCamera() {
	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(10.0f, 10.0f, 10.0f));
	cam.lookAt(glm::vec3(0.0f, 0.0f, 0.0f));
	cam.update(0.0);
	return cam;
}

static void addUnitCube(scenegraph::SceneGraph &sceneGraph) {
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 0, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	palette::Palette pal;
	pal.nippon();
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
}

TEST_F(PathTracerTest, testCreatePathTracerInterface) {
	voxelpathtracer::IPathTracer *tracer = voxelpathtracer::createPathTracer();
	ASSERT_TRUE(tracer != nullptr);
	EXPECT_FALSE(tracer->started());
	delete tracer;
}

TEST_F(PathTracerTest, testHideEnvironmentDefaultsOn) {
	voxelpathtracer::PathTracer yocto;
	voxelpathtracer::VoxelDDAPathTracer dda;
	EXPECT_TRUE(yocto.state().params.envhidden);
	EXPECT_TRUE(dda.state().params.envhidden);

	scenegraph::SceneGraph empty;
	yocto.applyAppearanceFromScene(empty);
	dda.applyAppearanceFromScene(empty);
	EXPECT_TRUE(yocto.state().params.envhidden);
	EXPECT_TRUE(dda.state().params.envhidden);
}

TEST_F(PathTracerTest, testAppearancePersistsOnScene) {
	scenegraph::SceneGraph sceneGraph;
	voxelpathtracer::PathTracer pathTracer;
	EXPECT_TRUE(pathTracer.state().params.envhidden);
	pathTracer.state().hdriEnvironment = true;
	pathTracer.state().hdriPath = "studio.hdr";
	pathTracer.state().hdriIntensity = 2.5f;
	pathTracer.state().hdriAzimuth = glm::radians(45.0f);
	pathTracer.state().groundPlane = true;
	pathTracer.state().studioEdges = true;
	pathTracer.state().params.envhidden = false;
	pathTracer.state().exposure = 0.75f;
	pathTracer.state().filmic = true;
	pathTracer.writeAppearanceToScene(sceneGraph);

	const scenegraph::SceneGraphNode &root = sceneGraph.root();
	EXPECT_EQ(root.property(scenegraph::PropHdri), "true");
	EXPECT_EQ(root.property(scenegraph::PropHdriPath), "studio.hdr");
	EXPECT_EQ(root.property(scenegraph::PropGroundPlane), "true");
	EXPECT_EQ(root.property(scenegraph::PropStudioEdges), "true");
	EXPECT_EQ(root.property(scenegraph::PropEnvHidden), "false");
	EXPECT_NEAR(root.property(scenegraph::PropRenderExposure).toFloat(), 0.75f, 0.01f);
	EXPECT_EQ(root.property(scenegraph::PropRenderFilmic), "true");

	pathTracer.state().resetAppearance();
	ASSERT_FALSE(pathTracer.state().hdriEnvironment);
	ASSERT_TRUE(pathTracer.state().params.envhidden);
	pathTracer.applyAppearanceFromScene(sceneGraph);
	EXPECT_TRUE(pathTracer.state().hdriEnvironment);
	EXPECT_EQ(pathTracer.state().hdriPath, "studio.hdr");
	EXPECT_NEAR(pathTracer.state().hdriIntensity, 2.5f, 0.01f);
	EXPECT_NEAR(pathTracer.state().hdriAzimuth, glm::radians(45.0f), 0.01f);
	EXPECT_TRUE(pathTracer.state().groundPlane);
	EXPECT_TRUE(pathTracer.state().studioEdges);
	EXPECT_FALSE(pathTracer.state().params.envhidden);
	EXPECT_NEAR(pathTracer.state().exposure, 0.75f, 0.01f);
	EXPECT_TRUE(pathTracer.state().filmic);
}

TEST_F(PathTracerTest, testHdriEnvironmentMap) {
	const core::String path = _testApp->filesystem()->homeWritePath("pathtracer_test.hdr");
	io::FilePtr file = core::make_shared<io::File>(path, io::FileMode::SysWrite);
	ASSERT_TRUE(file->validHandle());
	io::FileStream stream(file);
	const core::String header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 2\n";
	ASSERT_EQ(stream.write(header.c_str(), (int)header.size()), (int)header.size());
	const uint8_t px[16] = {255, 0, 0, 128, 0, 255, 0, 128, 0, 0, 255, 128, 255, 255, 255, 128};
	ASSERT_EQ(stream.write(px, sizeof(px)), (int)sizeof(px));
	stream.close();
	file = {};

	scenegraph::SceneGraph sceneGraph;
	sceneGraph.node(0).setProperty(scenegraph::PropHdri, true);
	sceneGraph.node(0).setProperty(scenegraph::PropHdriPath, path);
	video::Camera cam = testCamera();
	voxelpathtracer::PathTracer pathTracer;
	ASSERT_TRUE(pathTracer.start(sceneGraph, &cam));
	EXPECT_TRUE(pathTracer.state().hdriEnvironment);
	ASSERT_FALSE(pathTracer.state().scene.textures.empty());
	EXPECT_EQ(pathTracer.state().scene.textures[0].width, 2);
	EXPECT_EQ(pathTracer.state().scene.textures[0].height, 2);
	EXPECT_FALSE(pathTracer.state().scene.textures[0].pixelsf.empty());
	ASSERT_TRUE(pathTracer.stop());
}

TEST_F(PathTracerTest, testExrHdriLoadsAndBuildsCdf) {
	// Synthesize a small EXR, then load it through the same decode path the
	// renderer uses. Verify the pixels survive and the DDA tracer accepts it.
	const int w = 4;
	const int h = 2;
	core::Buffer<float> src((size_t)w * (size_t)h * 4u);
	for (int i = 0; i < w * h; ++i) {
		src[i * 4 + 0] = 1.0f;   // R
		src[i * 4 + 1] = 0.5f;   // G
		src[i * 4 + 2] = 0.25f;  // B
		src[i * 4 + 3] = 1.0f;   // A
	}
	unsigned char *exrMem = nullptr;
	const char *err = nullptr;
	const int exrSize = SaveEXRToMemory(src.data(), w, h, 4, 1, &exrMem, &err);
	ASSERT_GT(exrSize, 0) << (err != nullptr ? err : "save failed");

	const core::String path = _testApp->filesystem()->homeWritePath("pathtracer_test.exr");
	io::FilePtr file = core::make_shared<io::File>(path, io::FileMode::SysWrite);
	ASSERT_TRUE(file->validHandle());
	io::FileStream stream(file);
	ASSERT_EQ(exrSize, stream.write((const char *)exrMem, exrSize));
	stream.close();
	file = {};
	free(exrMem);

	core::Buffer<float> rgba;
	int outW = 0;
	int outH = 0;
	ASSERT_TRUE(voxelpathtracer::pathTracerLoadHdriFloats(path, rgba, outW, outH));
	EXPECT_EQ(w, outW);
	EXPECT_EQ(h, outH);
	ASSERT_EQ((size_t)w * (size_t)h * 4u, rgba.size());
	EXPECT_FLOAT_EQ(1.0f, rgba[0]);
	EXPECT_FLOAT_EQ(0.5f, rgba[1]);
	EXPECT_FLOAT_EQ(0.25f, rgba[2]);
	EXPECT_FLOAT_EQ(1.0f, rgba[3]);

	// The DDA tracer must load the .exr and build a finite luminance CDF.
	scenegraph::SceneGraph sceneGraph;
	sceneGraph.node(0).setProperty(scenegraph::PropHdri, true);
	sceneGraph.node(0).setProperty(scenegraph::PropHdriPath, path);
	video::Camera cam = testCamera();
	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 32;
	tracer.state().params.samples = 1;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	EXPECT_TRUE(tracer.state().hdriEnvironment);
	ASSERT_TRUE(tracer.stop());
}

TEST_F(PathTracerTest, testGroundPlaneAddsShape) {
	// Production ground is the analytic VoxelDDA plane, not Yocto mesh tris.
	// Look at the padded floor beside the cube so the center ray never hits
	// the model. Hidden environment makes a miss transparent.
	auto renderFloor = [this](bool ground) {
		scenegraph::SceneGraph sceneGraph;
		addUnitCube(sceneGraph);
		sceneGraph.node(0).setProperty(scenegraph::PropGroundPlane, ground ? "true" : "false");
		video::Camera cam;
		cam.setSize(glm::ivec2(32, 32));
		cam.setWorldPosition(glm::vec3(6.0f, 4.0f, 10.0f));
		cam.lookAt(glm::vec3(6.0f, 0.0f, 6.0f));
		cam.update(0.0);
		voxelpathtracer::VoxelDDAPathTracer tracer;
		tracer.state().params.resolution = 32;
		tracer.state().params.samples = 4;
		tracer.state().params.batch = 4;
		color::RGBA c(0, 0, 0, 0);
		if (!tracer.start(sceneGraph, &cam)) {
			return c;
		}
		EXPECT_EQ(ground, tracer.state().groundPlane);
		tracer.update();
		if (const image::ImagePtr img = tracer.image()) {
			c = img->colorAt(img->width() / 2, img->height() / 2);
		}
		tracer.stop();
		return c;
	};
	const color::RGBA withGround = renderFloor(true);
	const color::RGBA withoutGround = renderFloor(false);
	EXPECT_GT((int)withGround.a, 200) << "analytic ground should be an opaque hit";
	EXPECT_LT((int)withoutGround.a, 20) << "without the plane the same ray must miss";
	const int lo = glm::min((int)withGround.r, glm::min((int)withGround.g, (int)withGround.b));
	const int hi = glm::max((int)withGround.r, glm::max((int)withGround.g, (int)withGround.b));
	EXPECT_LE(hi - lo, 16) << "ground albedo is neutral grey, not the cube";
}

TEST_F(PathTracerTest, testStudioEdgesAddsRimTriangles) {
	scenegraph::SceneGraph sceneGraph;
	addUnitCube(sceneGraph);
	video::Camera cam = testCamera();
	voxelpathtracer::PathTracer plain;
	ASSERT_TRUE(plain.start(sceneGraph, &cam));
	int plainTris = 0;
	for (const yocto::shape_data &shape : plain.state().scene.shapes) {
		plainTris += (int)shape.triangles.size();
	}
	ASSERT_TRUE(plain.stop());

	sceneGraph.node(0).setProperty(scenegraph::PropStudioEdges, true);
	voxelpathtracer::PathTracer beveled;
	ASSERT_TRUE(beveled.start(sceneGraph, &cam));
	int bevelTris = 0;
	for (const yocto::shape_data &shape : beveled.state().scene.shapes) {
		bevelTris += (int)shape.triangles.size();
	}
	EXPECT_GT(bevelTris, plainTris);
	ASSERT_TRUE(beveled.stop());
}

TEST_F(PathTracerTest, testVoxelDDARendersUnitCube) {
	scenegraph::SceneGraph sceneGraph;
	addUnitCube(sceneGraph);
	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(2.0f, 1.6f, 2.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);
	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 8;
	tracer.state().params.batch = 8;
	tracer.state().params.bounces = 1;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	while (!tracer.update()) {
	}
	const image::ImagePtr img = tracer.image();
	ASSERT_TRUE(img);
	ASSERT_TRUE(img->isLoaded());
	EXPECT_EQ(64, img->width());
	const color::RGBA c = img->colorAt(img->width() / 2, img->height() / 2);
	const int l = ((int)c.r + (int)c.g + (int)c.b) / 3;
	// Unshadowed IBL must color the cube. A black cutout fails this.
	EXPECT_GT((int)c.a, 200);
	EXPECT_GT(l, 60);
	ASSERT_TRUE(tracer.stop());
}

TEST_F(PathTracerTest, testVoxelDDAMidGrayUsesLinearLightingOnce) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 0, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(128, 128, 128, 255));
	pal.setRoughness(1, 1.0f);
	pal.setSpecular(1, 0.0f);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(2.0f, 1.5f, 2.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 256;
	tracer.state().params.batch = 256;
	tracer.state().params.bounces = 1;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	ASSERT_TRUE(tracer.update());
	const image::ImagePtr img = tracer.image();
	ASSERT_TRUE(img);
	const color::RGBA c = img->colorAt(img->width() / 2, img->height() / 2);
	const int luma = ((int)c.r + (int)c.g + (int)c.b) / 3;
	// Palette bytes are sRGB. A mid-gray diffuse face under the 0.91 studio
	// environment should remain near mid-gray. Treating 128 as linear and
	// adding the environment both directly and through the bounce made it white.
	EXPECT_GT(luma, 100) << "luma=" << luma;
	EXPECT_LT(luma, 160) << "luma=" << luma;
	ASSERT_TRUE(tracer.stop());
}

// Renders a large flat diffuse wall that fills the frame and returns the center
// pixel luma plus the final sample count. Used to prove adaptive sampling stops
// a converged scene early and still matches the full-sample mean.
static int flatWallCenterLuma(bool adaptive, int samples, int &finalSample) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 15, 15, 0));
	for (int y = 0; y < 16; ++y) {
		for (int x = 0; x < 16; ++x) {
			v->setVoxel(x, y, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
		}
	}
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(128, 128, 128, 255));
	pal.setRoughness(1, 1.0f);
	pal.setSpecular(1, 0.0f);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(8.0f, 8.0f, 6.0f));
	cam.lookAt(glm::vec3(8.0f, 8.0f, 0.0f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = samples;
	tracer.state().params.batch = 8;
	tracer.state().params.bounces = 1;
	tracer.state().params.adaptive = adaptive;
	tracer.state().params.adaptiveError = 0.02f;
	if (!tracer.start(sceneGraph, &cam)) {
		return -1;
	}
	while (!tracer.update(&finalSample)) {
	}
	const image::ImagePtr img = tracer.image();
	if (!img || !img->isLoaded()) {
		return -1;
	}
	const color::RGBA c = img->colorAt(img->width() / 2, img->height() / 2);
	tracer.stop();
	return ((int)c.r + (int)c.g + (int)c.b) / 3;
}

TEST_F(PathTracerTest, testVoxelDDAAdaptiveSamplingStopsFlatSceneEarly) {
	int finalSample = 0;
	const int luma = flatWallCenterLuma(true, 512, finalSample);
	// A constant flat wall converges to the same radiance everywhere, so every
	// pixel reaches the relative-error floor after the minimum sample count and
	// the render terminates well before the 512-sample safety cap.
	EXPECT_GE(finalSample, 16) << "stopped before the minimum sample floor";
	EXPECT_LT(finalSample, 128) << "adaptive sampling did not stop early (sample=" << finalSample << ")";
	EXPECT_GT(luma, 60) << "flat wall should be lit, luma=" << luma;
}

TEST_F(PathTracerTest, testVoxelDDAAdaptiveSamplingMatchesFullSampling) {
	int adaptiveSample = 0;
	const int adaptiveLuma = flatWallCenterLuma(true, 256, adaptiveSample);
	int fullSample = 0;
	const int fullLuma = flatWallCenterLuma(false, 256, fullSample);
	ASSERT_GE(adaptiveLuma, 0);
	ASSERT_GE(fullLuma, 0);
	// Adaptive sampling stops far earlier than the full budget...
	EXPECT_LT(adaptiveSample, fullSample);
	// ...yet the converged flat-wall mean matches the full render closely.
	EXPECT_NEAR(fullLuma, adaptiveLuma, 8.0f) << "adaptive=" << adaptiveLuma << " full=" << fullLuma;
}

static int neighborMad(const image::ImagePtr &img) {
	if (!img) {
		return -1;
	}
	int sum = 0;
	int n = 0;
	const int w = img->width();
	const int h = img->height();
	for (int y = h * 3 / 8; y < h * 5 / 8; ++y) {
		for (int x = w * 3 / 8; x < w * 5 / 8; ++x) {
			const color::RGBA c = img->colorAt(x, y);
			const color::RGBA r = img->colorAt(x + 1, y);
			if (c.a < 8 || r.a < 8) {
				continue;
			}
			sum += glm::abs((int)c.r - (int)r.r) + glm::abs((int)c.g - (int)r.g) + glm::abs((int)c.b - (int)r.b);
			++n;
		}
	}
	return n > 0 ? sum / n : 0;
}

static image::ImagePtr renderDdaCube(bool denoise, int samples) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 0, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(200, 180, 160, 255));
	pal.setRoughness(1, 1.0f);
	pal.setSpecular(1, 0.2f);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(0).setProperty(scenegraph::PropEnvHidden, "false");
	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(0.5f, 0.5f, 3.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);
	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = samples;
	tracer.state().params.batch = samples;
	tracer.state().params.bounces = 2;
	tracer.state().params.denoise = denoise;
	if (!tracer.start(sceneGraph, &cam)) {
		return {};
	}
	tracer.state().params.envhidden = false;
	tracer.state().params.denoise = denoise;
	tracer.update();
	image::ImagePtr img = tracer.image();
	tracer.stop();
	return img;
}

TEST_F(PathTracerTest, testVoxelDDADenoiseReducesNoise) {
	const image::ImagePtr raw = renderDdaCube(false, 2);
	const image::ImagePtr den = renderDdaCube(true, 2);
	ASSERT_TRUE(raw);
	ASSERT_TRUE(den);
	const int rawMad = neighborMad(raw);
	const int denMad = neighborMad(den);
	ASSERT_GE(rawMad, 0);
	ASSERT_GE(denMad, 0);
	EXPECT_LT(denMad, rawMad) << "den=" << denMad << " raw=" << rawMad;
}

TEST_F(PathTracerTest, testVoxelDDATemporalDenoiseConverges) {
	scenegraph::SceneGraph sceneGraph;
	addUnitCube(sceneGraph);
	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(2.0f, 1.6f, 2.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 16;
	tracer.state().params.batch = 4;
	tracer.state().params.bounces = 1;
	tracer.state().params.denoise = true;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));

	// Render progressively, denoising after each batch so the SVGF temporal
	// history accumulates across frames.
	int centerLuma[4] = {0, 0, 0, 0};
	int step = 0;
	while (step < 4) {
		const bool done = tracer.update();
		const image::ImagePtr img = tracer.image();
		ASSERT_TRUE(img);
		ASSERT_TRUE(img->isLoaded());
		const color::RGBA c = img->colorAt(img->width() / 2, img->height() / 2);
		centerLuma[step] = ((int)c.r + (int)c.g + (int)c.b) / 3;
		++step;
		if (done) {
			break;
		}
	}
	ASSERT_EQ(4, step) << "expected four progressive batches";
	for (int s = 0; s < step; ++s) {
		EXPECT_GT(centerLuma[s], 40) << "frame " << s << " went dark (luma=" << centerLuma[s] << ")";
		EXPECT_LT(centerLuma[s], 250) << "frame " << s << " clipped (luma=" << centerLuma[s] << ")";
	}
	// The temporally accumulated estimate converges; later frames stop moving.
	EXPECT_LE(glm::abs(centerLuma[3] - centerLuma[2]), 8)
		<< "temporal denoise did not converge (frames " << centerLuma[2] << " -> " << centerLuma[3] << ")";
	ASSERT_TRUE(tracer.stop());
}

TEST_F(PathTracerTest, testVoxelDDATemporalDenoiseResetsOnRestart) {
	scenegraph::SceneGraph sceneGraph;
	addUnitCube(sceneGraph);
	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(2.0f, 1.6f, 2.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 8;
	tracer.state().params.batch = 4;
	tracer.state().params.bounces = 1;
	tracer.state().params.denoise = true;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	ASSERT_FALSE(tracer.update());
	const image::ImagePtr first = tracer.image();
	ASSERT_TRUE(first && first->isLoaded());
	const color::RGBA a = first->colorAt(first->width() / 2, first->height() / 2);
	// Restart clears accumulation and the temporal history.
	ASSERT_TRUE(tracer.restart(sceneGraph, &cam));
	ASSERT_FALSE(tracer.update());
	const image::ImagePtr second = tracer.image();
	ASSERT_TRUE(second && second->isLoaded());
	const color::RGBA b = second->colorAt(second->width() / 2, second->height() / 2);
	// A fresh render after restart reproduces the seeded (spatial-only) result
	// exactly, proving no stale temporal history leaked across the restart.
	EXPECT_EQ((int)a.r, (int)b.r);
	EXPECT_EQ((int)a.g, (int)b.g);
	EXPECT_EQ((int)a.b, (int)b.b);
	ASSERT_TRUE(tracer.stop());
}

static int maxRowJump(const image::ImagePtr &img) {
	int maxJump = 0;
	const int y = img->height() / 2;
	for (int x = 2; x < img->width() - 2; ++x) {
		const color::RGBA a = img->colorAt(x, y);
		const color::RGBA b = img->colorAt(x + 1, y);
		const int da = ((int)a.r + (int)a.g + (int)a.b) / 3;
		const int db = ((int)b.r + (int)b.g + (int)b.b) / 3;
		maxJump = glm::max(maxJump, da < db ? db - da : da - db);
	}
	return maxJump;
}

TEST_F(PathTracerTest, testVoxelDDADenoiseKeepsEdge) {
	const image::ImagePtr raw = renderDdaCube(false, 8);
	const image::ImagePtr den = renderDdaCube(true, 8);
	ASSERT_TRUE(raw);
	ASSERT_TRUE(den);
	const int jumpRaw = maxRowJump(raw);
	const int jumpDen = maxRowJump(den);
	// Raw max jump is often a spec highlight on one face. Denoise flattens
	// that (same albedo + normal) but must not erase the silhouette.
	EXPECT_GT(jumpDen, 8) << "jumpDen=" << jumpDen << " jumpRaw=" << jumpRaw;
	(void)jumpRaw;
}

TEST_F(PathTracerTest, testVoxelDDADenoiseKeepsVoxelEdges) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 3, 3, 0));
	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			v->setVoxel(x, y, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
		}
	}
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(200, 190, 170, 255));
	pal.setRoughness(1, 1.0f);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(0).setProperty(scenegraph::PropEnvHidden, true);
	sceneGraph.node(0).setProperty(scenegraph::PropStudioEdges, true);

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(2.0f, 2.0f, 10.0f));
	cam.lookAt(glm::vec3(2.0f, 2.0f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 64;
	tracer.state().params.batch = 64;
	tracer.state().params.bounces = 1;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	ASSERT_TRUE(tracer.update());
	tracer.state().params.denoise = false;
	const image::ImagePtr raw = tracer.image();
	tracer.state().params.denoise = true;
	const image::ImagePtr denoised = tracer.image();
	ASSERT_TRUE(raw);
	ASSERT_TRUE(denoised);

	const int y = denoised->height() / 2;
	int rawMin = 255;
	int rawMax = 0;
	int denoisedMin = 255;
	int denoisedMax = 0;
	int pixels = 0;
	for (int x = 3; x < denoised->width() - 3; ++x) {
		if (denoised->colorAt(x - 3, y).a < 250 || denoised->colorAt(x + 3, y).a < 250) {
			continue;
		}
		const color::RGBA r = raw->colorAt(x, y);
		const color::RGBA d = denoised->colorAt(x, y);
		const int rl = ((int)r.r + (int)r.g + (int)r.b) / 3;
		const int dl = ((int)d.r + (int)d.g + (int)d.b) / 3;
		rawMin = glm::min(rawMin, rl);
		rawMax = glm::max(rawMax, rl);
		denoisedMin = glm::min(denoisedMin, dl);
		denoisedMax = glm::max(denoisedMax, dl);
		++pixels;
	}
	ASSERT_GT(pixels, 8);
	EXPECT_GT(denoisedMax - denoisedMin, 8) << "min=" << denoisedMin << " max=" << denoisedMax;
	EXPECT_GT((denoisedMax - denoisedMin) * 2, rawMax - rawMin)
		<< "raw=" << rawMin << "/" << rawMax << " denoised=" << denoisedMin << "/" << denoisedMax;
	ASSERT_TRUE(tracer.stop());
}

static void addLampOverGround(scenegraph::SceneGraph &sceneGraph, float emit) {
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 4, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	v->setVoxel(0, 4, 0, voxel::createVoxel(voxel::VoxelType::Generic, 2));
	palette::Palette pal;
	pal.nippon();
	if (emit > 0.0f) {
		pal.setEmit(2, emit);
	}
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(0).setProperty(scenegraph::PropGroundPlane, true);
}

TEST_F(PathTracerTest, testVoxelDDAEmitLightsNearbySurface) {
	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(6.0f, 2.0f, 6.0f));
	cam.lookAt(glm::vec3(0.5f, 0.0f, 0.5f));
	cam.update(0.0);

	auto groundLuma = [&](float emit, int bounces, int samples, float exposure) {
		scenegraph::SceneGraph sceneGraph;
		addLampOverGround(sceneGraph, emit);
		voxelpathtracer::VoxelDDAPathTracer tracer;
		tracer.state().params.resolution = 64;
		tracer.state().params.samples = samples;
		tracer.state().params.batch = samples;
		tracer.state().params.bounces = bounces;
		tracer.state().exposure = exposure;
		tracer.start(sceneGraph, &cam);
		tracer.update();
		const image::ImagePtr img = tracer.image();
		int l = 0;
		if (img) {
			const color::RGBA c = img->colorAt(img->width() / 2, img->height() * 3 / 4);
			l = ((int)c.r + (int)c.g + (int)c.b) / 3;
		}
		tracer.stop();
		return l;
	};

	EXPECT_GT(groundLuma(1.0f, 2, 24, -2.0f), groundLuma(0.0f, 2, 24, -2.0f));
	// With another bounce available, direct emitter sampling and BSDF hits
	// are combined with MIS. The result must retain the one-bounce energy,
	// not double count the lamp or lose BSDF paths that land on it.
	const int directOnly = groundLuma(0.35f, 1, 256, -3.0f);
	const int withMis = groundLuma(0.35f, 2, 256, -3.0f);
	EXPECT_NEAR(directOnly, withMis, 18) << "direct=" << directOnly << " mis=" << withMis;
}

static int separatedLampGroundLuma(bool blockerBehindLight) {
	scenegraph::SceneGraph sceneGraph;
	auto addVoxel = [&](const glm::vec3 &translation, const color::RGBA &color, float emit) {
		scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
		voxel::RawVolume *volume = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 0, 0));
		volume->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
		palette::Palette pal;
		pal.nippon();
		pal.setColor(1, color);
		if (emit > 0.0f) {
			pal.setEmit(1, emit);
		}
		node.setPivot(glm::vec3(0.0f));
		node.transform(0).setLocalTranslation(translation);
		node.setPalette(pal);
		node.setVolume(volume);
		sceneGraph.emplace(core::move(node));
	};
	// Put this grid first to exercise traversal order. It is above the
	// finite lamp and must not shadow a receiver below the lamp.
	if (blockerBehindLight) {
		addVoxel(glm::vec3(0.0f, 8.0f, 0.0f), color::RGBA(32, 32, 32, 255), 0.0f);
	}
	addVoxel(glm::vec3(0.0f), color::RGBA(180, 170, 150, 255), 0.0f);
	addVoxel(glm::vec3(0.0f, 4.0f, 0.0f), color::RGBA(255, 240, 210, 255), 0.55f);
	sceneGraph.updateTransforms();
	sceneGraph.node(0).setProperty(scenegraph::PropGroundPlane, true);

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(6.0f, 2.0f, 6.0f));
	cam.lookAt(glm::vec3(0.5f, 0.0f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 64;
	tracer.state().params.batch = 64;
	tracer.state().params.bounces = 1;
	tracer.state().exposure = -2.0f;
	if (!tracer.start(sceneGraph, &cam)) {
		return -1;
	}
	tracer.update();
	const image::ImagePtr img = tracer.image();
	int luma = -1;
	if (img) {
		const color::RGBA c = img->colorAt(img->width() / 2, img->height() * 3 / 4);
		luma = ((int)c.r + (int)c.g + (int)c.b) / 3;
	}
	tracer.stop();
	return luma;
}

TEST_F(PathTracerTest, testVoxelDDAObjectBehindEmitterDoesNotShadowIt) {
	const int clear = separatedLampGroundLuma(false);
	const int blockedBehind = separatedLampGroundLuma(true);
	ASSERT_GT(clear, 0);
	ASSERT_GT(blockedBehind, 0);
	EXPECT_NEAR(clear, blockedBehind, 2) << "clear=" << clear << " behind=" << blockedBehind;
}

TEST_F(PathTracerTest, testVoxelDDAAlphaIsSeeThrough) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 0, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(255, 0, 0, 40));
	pal.setMaterialValue(1, palette::MaterialIndexOfRefraction, 1.0f);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(0).setProperty(scenegraph::PropEnvHidden, false);

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(2.0f, 1.6f, 2.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 32;
	tracer.state().params.batch = 32;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	ASSERT_TRUE(tracer.update());
	const image::ImagePtr &img = tracer.image();
	ASSERT_TRUE(img);
	const color::RGBA c = img->colorAt(img->width() / 2, img->height() / 2);
	const int l = ((int)c.r + (int)c.g + (int)c.b) / 3;
	// Mostly see-through: studio wrap should dominate over solid red.
	EXPECT_GT(l, 80);
	EXPECT_GT((int)c.r, (int)c.b);
	ASSERT_TRUE(tracer.stop());
}

TEST_F(PathTracerTest, testVoxelDDAAppliesHdriAzimuthFromScene) {
	scenegraph::SceneGraph sceneGraph;
	addUnitCube(sceneGraph);
	sceneGraph.node(0).setProperty(scenegraph::PropHdriAzimuth, 135.0f);
	video::Camera cam = testCamera();
	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 16;
	tracer.state().params.samples = 1;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	EXPECT_NEAR(tracer.state().hdriAzimuth, glm::radians(135.0f), 0.01f);
	ASSERT_TRUE(tracer.stop());
}

TEST_F(PathTracerTest, testVoxelDDAStudioBackgroundIsBright) {
	scenegraph::SceneGraph sceneGraph;
	sceneGraph.node(0).setProperty(scenegraph::PropEnvHidden, false);
	video::Camera cam = testCamera();
	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 32;
	tracer.state().params.samples = 2;
	tracer.state().params.batch = 2;
	tracer.state().params.bounces = 1;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	ASSERT_TRUE(tracer.update());
	const image::ImagePtr &img = tracer.image();
	ASSERT_TRUE(img);
	ASSERT_TRUE(img->isLoaded());
	const color::RGBA c = img->colorAt(img->width() / 2, img->height() / 2);
	// Yocto-style sRGB tonemap of the studio wrap is near-white. Reinhard
	// crushed this into mid greys (around 170).
	EXPECT_GT((int)c.r, 200);
	EXPECT_GT((int)c.g, 200);
	EXPECT_GT((int)c.b, 200);
	ASSERT_TRUE(tracer.stop());
}

TEST_F(PathTracerTest, testVoxelDDAHideEnvironmentIsTransparent) {
	scenegraph::SceneGraph sceneGraph;
	video::Camera cam = testCamera();
	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 32;
	tracer.state().params.samples = 2;
	tracer.state().params.batch = 2;
	ASSERT_TRUE(tracer.state().params.envhidden);
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	ASSERT_TRUE(tracer.update());
	const image::ImagePtr &img = tracer.image();
	ASSERT_TRUE(img);
	const color::RGBA c = img->colorAt(img->width() / 2, img->height() / 2);
	EXPECT_EQ((int)c.a, 0);
	ASSERT_TRUE(tracer.stop());
}

static core::String writeSpotHdri(const io::FilesystemPtr &fs, const char *name, int sunX, int sunY,
								 uint8_t sunMantissa = 220, uint8_t sunExponent = 136) {
	const int w = 16;
	const int h = 16;
	const core::String path = fs->homeWritePath(name);
	io::FilePtr file = core::make_shared<io::File>(path, io::FileMode::SysWrite);
	if (!file->validHandle()) {
		return "";
	}
	io::FileStream stream(file);
	const core::String header = core::String::format("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %i +X %i\n", h, w);
	if (stream.write(header.c_str(), (int)header.size()) != (int)header.size()) {
		return "";
	}
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const bool sun = (x == sunX && y == sunY);
			const uint8_t px[4] = {sun ? sunMantissa : (uint8_t)4, sun ? sunMantissa : (uint8_t)4,
								   sun ? sunMantissa : (uint8_t)4, sun ? sunExponent : (uint8_t)128};
			if (stream.write(px, 4) != 4) {
				return "";
			}
		}
	}
	stream.close();
	return path;
}

TEST_F(PathTracerTest, testVoxelDDAGroundReceivesOcclusion) {
	// Above the +X horizon (u=0, v=0.25) so the cube casts a stable
	// left/right shadow on the ground. An equatorial texel straddles the
	// horizon when jittered and makes the test measure sampling noise.
	const core::String hdriPath = writeSpotHdri(_testApp->filesystem(), "pathtracer_sun.hdr", 0, 4);
	ASSERT_FALSE(hdriPath.empty());

	scenegraph::SceneGraph sceneGraph;
	addUnitCube(sceneGraph);
	sceneGraph.node(0).setProperty(scenegraph::PropGroundPlane, true);
	sceneGraph.node(0).setProperty(scenegraph::PropHdri, true);
	sceneGraph.node(0).setProperty(scenegraph::PropHdriPath, hdriPath);

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(0.5f, 3.0f, 8.0f));
	cam.lookAt(glm::vec3(0.5f, 0.0f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 64;
	tracer.state().params.batch = 64;
	tracer.state().params.bounces = 1;
	tracer.state().exposure = -8.0f;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	ASSERT_TRUE(tracer.update());
	const image::ImagePtr &img = tracer.image();
	ASSERT_TRUE(img);

	// A directional HDRI texel makes unoccluded floor irradiance constant.
	// Select opaque neutral-grey pixels so perspective may move the shadow
	// without accidentally measuring the colored cube or transparent sky.
	int shadowL = 255;
	int litL = 0;
	int groundPixels = 0;
	for (int y = 0; y < img->height(); ++y) {
		for (int x = 0; x < img->width(); ++x) {
			const color::RGBA c = img->colorAt(x, y);
			const int lo = glm::min((int)c.r, glm::min((int)c.g, (int)c.b));
			const int hi = glm::max((int)c.r, glm::max((int)c.g, (int)c.b));
			if (c.a < 250 || hi - lo > 2) {
				continue;
			}
			const int value = ((int)c.r + (int)c.g + (int)c.b) / 3;
			shadowL = glm::min(shadowL, value);
			litL = glm::max(litL, value);
			++groundPixels;
		}
	}
	ASSERT_GT(groundPixels, 32);
	EXPECT_GT(litL - shadowL, 5) << "shadow=" << shadowL << " lit=" << litL;

	ASSERT_TRUE(tracer.stop());
}

TEST_F(PathTracerTest, testVoxelDDAAppearanceAndGround) {
	scenegraph::SceneGraph sceneGraph;
	addUnitCube(sceneGraph);
	sceneGraph.node(0).setProperty(scenegraph::PropGroundPlane, true);
	sceneGraph.node(0).setProperty(scenegraph::PropStudioEdges, true);
	video::Camera cam = testCamera();
	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 32;
	tracer.state().params.samples = 1;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	EXPECT_TRUE(tracer.state().groundPlane);
	EXPECT_TRUE(tracer.state().studioEdges);
	ASSERT_TRUE(tracer.update());
	ASSERT_TRUE(tracer.image());
	ASSERT_TRUE(tracer.stop());
}

static int groundLumaUnderFloater(const core::String &hdriPath, bool floater, uint8_t alpha, float atten) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 12, 4, 12));
	v->setVoxel(12, 0, 12, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	if (floater) {
		v->setVoxel(0, 4, 0, voxel::createVoxel(voxel::VoxelType::Generic, 2));
	}
	palette::Palette pal;
	pal.nippon();
	pal.setColor(2, color::RGBA(255, 32, 32, alpha));
	if (atten > 0.0f) {
		pal.setAttenuation(2, atten);
	}
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(0).setProperty(scenegraph::PropGroundPlane, true);
	sceneGraph.node(0).setProperty(scenegraph::PropHdri, true);
	sceneGraph.node(0).setProperty(scenegraph::PropHdriPath, hdriPath);

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(0.5f, 1.2f, 8.0f));
	cam.lookAt(glm::vec3(0.5f, 0.0f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 32;
	tracer.state().params.batch = 32;
	tracer.state().params.bounces = 1;
	tracer.state().exposure = -1.0f;
	if (!tracer.start(sceneGraph, &cam)) {
		return -1;
	}
	tracer.update();
	const image::ImagePtr img = tracer.image();
	int l = -1;
	if (img) {
		const int cx = img->width() / 2;
		const int cy = img->height() / 2;
		int sum = 0;
		int n = 0;
		for (int dy = -1; dy <= 1; ++dy) {
			for (int dx = -1; dx <= 1; ++dx) {
				const color::RGBA c = img->colorAt(cx + dx, cy + dy);
				sum += ((int)c.r + (int)c.g + (int)c.b) / 3;
				++n;
			}
		}
		l = sum / n;
	}
	tracer.stop();
	return l;
}

TEST_F(PathTracerTest, testVoxelDDAAlphaAttenuatesShadows) {
	// +Y pole so the floater sits on the sun ray that hits the ground target.
	const core::String hdriPath = writeSpotHdri(_testApp->filesystem(), "pathtracer_up.hdr", 0, 0);
	ASSERT_FALSE(hdriPath.empty());

	const int openL = groundLumaUnderFloater(hdriPath, false, 80, 0.0f);
	const int clearL = groundLumaUnderFloater(hdriPath, true, 80, 0.0f);
	const int stainL = groundLumaUnderFloater(hdriPath, true, 80, 1.0f);
	const int blockL = groundLumaUnderFloater(hdriPath, true, 255, 0.0f);
	ASSERT_GE(openL, 0);
	ASSERT_GE(clearL, 0);
	ASSERT_GE(stainL, 0);
	ASSERT_GE(blockL, 0);
	// Open ground is brightest. Clear voxels pass a fraction of the sun.
	// Raising attenuation or going fully opaque darkens the umbra further.
	EXPECT_GT(openL, clearL) << "open=" << openL << " clear=" << clearL;
	EXPECT_GT(clearL, stainL) << "clear=" << clearL << " stain=" << stainL;
	EXPECT_GT(clearL, blockL) << "clear=" << clearL << " block=" << blockL;
}

struct MetalFaceLuma {
	int center = -1;
	int ring = -1;
	int peak() const {
		return center - ring;
	}
};

static MetalFaceLuma metalCubeHighlight(const core::String &hdriPath, float metal, float rough, bool typeMetal) {
	MetalFaceLuma out;
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 0, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(200, 160, 80, 255));
	if (typeMetal) {
		pal.setMaterialType(1, palette::MaterialType::Metal);
	}
	if (metal >= 0.0f) {
		pal.setMetal(1, metal);
	}
	pal.setRoughness(1, rough);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(0).setProperty(scenegraph::PropHdri, true);
	sceneGraph.node(0).setProperty(scenegraph::PropHdriPath, hdriPath);

	// Look at the +Z face. The HDRI spot is +Z, so a polished metal
	// should show a highlight in the image center.
	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(0.5f, 0.5f, 3.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 48;
	tracer.state().params.batch = 48;
	tracer.state().params.bounces = 2;
	tracer.state().exposure = -6.0f;
	if (!tracer.start(sceneGraph, &cam)) {
		return out;
	}
	tracer.update();
	const image::ImagePtr img = tracer.image();
	if (img) {
		auto luma = [&](int x, int y) {
			const color::RGBA c = img->colorAt(x, y);
			return ((int)c.r + (int)c.g + (int)c.b) / 3;
		};
		const int cx = img->width() / 2;
		const int cy = img->height() / 2;
		out.center = luma(cx, cy);
		out.ring = (luma(cx - 6, cy) + luma(cx + 6, cy) + luma(cx, cy - 6) + luma(cx, cy + 6)) / 4;
	}
	tracer.stop();
	return out;
}

TEST_F(PathTracerTest, testVoxelDDAMetalIsShiny) {
	// +Z equator (u=0.25, v=0.5) on a 16x16 lat-long.
	const core::String hdriPath = writeSpotHdri(_testApp->filesystem(), "pathtracer_metal.hdr", 4, 8);
	ASSERT_FALSE(hdriPath.empty());

	const MetalFaceLuma diffuse = metalCubeHighlight(hdriPath, 0.0f, 0.1f, false);
	const MetalFaceLuma diffuseRough = metalCubeHighlight(hdriPath, 0.0f, 1.0f, false);
	const MetalFaceLuma polish = metalCubeHighlight(hdriPath, 1.0f, 0.08f, false);
	const MetalFaceLuma dull = metalCubeHighlight(hdriPath, 1.0f, 0.9f, false);
	const MetalFaceLuma typeMetal = metalCubeHighlight(hdriPath, -1.0f, 0.08f, true);
	ASSERT_GE(diffuse.center, 0);
	ASSERT_GE(polish.center, 0);
	ASSERT_GE(dull.center, 0);
	ASSERT_GE(typeMetal.center, 0);
	// Default roughness on a non-metal must stay matte (no highlight).
	EXPECT_NEAR(diffuse.center, diffuseRough.center, 35);
	EXPECT_LT(diffuse.peak(), 25);
	// Polished metal has a tight highlight; rough metal is flatter.
	EXPECT_GT(polish.peak(), diffuse.peak())
		<< "polish c/r=" << polish.center << "/" << polish.ring << " diffuse c/r=" << diffuse.center << "/"
		<< diffuse.ring;
	EXPECT_GT(polish.peak(), dull.peak())
		<< "polish c/r=" << polish.center << "/" << polish.ring << " dull c/r=" << dull.center << "/" << dull.ring;
	// Magica type Metal with the slider at 0 is still a metal.
	EXPECT_GT(typeMetal.peak(), diffuse.peak())
		<< "typeMetal c/r=" << typeMetal.center << "/" << typeMetal.ring;
}

TEST_F(PathTracerTest, testVoxelDDAMediaIsSeeThrough) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 0, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(255, 0, 0, 255));
	pal.setMaterialType(1, palette::MaterialType::Volumetric);
	pal.setDensity(1, 0.35f);
	pal.setScatter(1, 0.85f);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(0).setProperty(scenegraph::PropEnvHidden, false);

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(2.0f, 1.6f, 2.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 32;
	tracer.state().params.batch = 32;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	ASSERT_TRUE(tracer.update());
	const image::ImagePtr &img = tracer.image();
	ASSERT_TRUE(img);
	const color::RGBA c = img->colorAt(img->width() / 2, img->height() / 2);
	const int l = ((int)c.r + (int)c.g + (int)c.b) / 3;
	// Fog, not a solid red cube: studio wrap shows through.
	EXPECT_GT(l, 50);
	EXPECT_GT((int)c.r, (int)c.b);
	ASSERT_TRUE(tracer.stop());
}

static int mediaEmitLuma(float emit) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 0, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(255, 80, 20, 255));
	pal.setMaterialType(1, palette::MaterialType::Volumetric);
	pal.setDensity(1, 0.5f);
	pal.setScatter(1, 0.15f);
	if (emit > 0.0f) {
		pal.setEmit(1, emit);
	}
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(0.5f, 0.5f, 3.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 24;
	tracer.state().params.batch = 24;
	tracer.state().exposure = -2.0f;
	if (!tracer.start(sceneGraph, &cam)) {
		return -1;
	}
	tracer.update();
	const image::ImagePtr img = tracer.image();
	int l = -1;
	if (img) {
		const color::RGBA c = img->colorAt(img->width() / 2, img->height() / 2);
		l = ((int)c.r + (int)c.g + (int)c.b) / 3;
	}
	tracer.stop();
	return l;
}

TEST_F(PathTracerTest, testVoxelDDAMediaEmitGlows) {
	const int dark = mediaEmitLuma(0.0f);
	const int hot = mediaEmitLuma(1.0f);
	ASSERT_GE(dark, 0);
	ASSERT_GE(hot, 0);
	EXPECT_GT(hot, dark) << "hot=" << hot << " dark=" << dark;
}

TEST_F(PathTracerTest, testVoxelDDAMediaEmitIsSoft) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 0, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(255, 0, 0, 255));
	pal.setMaterialType(1, palette::MaterialType::Volumetric);
	pal.setDensity(1, 0.08f);
	pal.setScatter(1, 0.0f);
	pal.setEmit(1, 0.18f);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(0).setProperty(scenegraph::PropEnvHidden, false);

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(0.5f, 0.5f, 3.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 24;
	tracer.state().params.batch = 24;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	tracer.state().params.envhidden = false;
	tracer.state().exposure = -1.0f;
	ASSERT_TRUE(tracer.update());
	const image::ImagePtr &img = tracer.image();
	ASSERT_TRUE(img);
	const color::RGBA c = img->colorAt(img->width() / 2, img->height() / 2);
	// Modest emit + thin density is a soft flame, not a clipped neon cube.
	EXPECT_LT((int)c.r, 250);
	EXPECT_GT(((int)c.r + (int)c.g + (int)c.b) / 3, 15);
	ASSERT_TRUE(tracer.stop());
}

static int mediaLampGroundLuma(bool volumetric, float emit) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 4, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	v->setVoxel(0, 4, 0, voxel::createVoxel(voxel::VoxelType::Generic, 2));
	palette::Palette pal;
	pal.nippon();
	pal.setColor(2, color::RGBA(255, 0, 0, 255));
	if (volumetric) {
		pal.setMaterialType(2, palette::MaterialType::Volumetric);
		pal.setDensity(2, 0.5f);
		pal.setScatter(2, 0.15f);
	}
	if (emit > 0.0f) {
		pal.setEmit(2, emit);
	}
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(0).setProperty(scenegraph::PropGroundPlane, true);

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(6.0f, 2.0f, 6.0f));
	cam.lookAt(glm::vec3(0.5f, 0.0f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 24;
	tracer.state().params.batch = 24;
	tracer.state().exposure = -2.0f;
	if (!tracer.start(sceneGraph, &cam)) {
		return -1;
	}
	tracer.update();
	const image::ImagePtr img = tracer.image();
	int l = -1;
	if (img) {
		const color::RGBA c = img->colorAt(img->width() / 2, img->height() * 3 / 4);
		l = ((int)c.r + (int)c.g + (int)c.b) / 3;
	}
	tracer.stop();
	return l;
}

TEST_F(PathTracerTest, testVoxelDDAMediaEmitDoesNotAreaLightGround) {
	const int mediaOff = mediaLampGroundLuma(true, 0.0f);
	const int mediaHot = mediaLampGroundLuma(true, 1.0f);
	const int solidHot = mediaLampGroundLuma(false, 1.0f);
	ASSERT_GE(mediaOff, 0);
	ASSERT_GE(mediaHot, 0);
	ASSERT_GE(solidHot, 0);
	EXPECT_GT(solidHot, mediaHot + 15) << "solid=" << solidHot << " media=" << mediaHot;
	EXPECT_LT(mediaHot - mediaOff, 20) << "mediaHot=" << mediaHot << " mediaOff=" << mediaOff;
}

static int mediaFloaterGroundLuma(const core::String &hdriPath, bool floater, float density, float phase) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 12, 4, 12));
	v->setVoxel(12, 0, 12, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	if (floater) {
		v->setVoxel(0, 4, 0, voxel::createVoxel(voxel::VoxelType::Generic, 2));
	}
	palette::Palette pal;
	pal.nippon();
	pal.setColor(2, color::RGBA(220, 220, 220, 255));
	pal.setMaterialType(2, palette::MaterialType::Volumetric);
	pal.setDensity(2, density);
	pal.setRimLight(2, phase);
	pal.setScatter(2, 0.9f);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(0).setProperty(scenegraph::PropGroundPlane, true);
	sceneGraph.node(0).setProperty(scenegraph::PropHdri, true);
	sceneGraph.node(0).setProperty(scenegraph::PropHdriPath, hdriPath);

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(0.5f, 1.2f, 8.0f));
	cam.lookAt(glm::vec3(0.5f, 0.0f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 32;
	tracer.state().params.batch = 32;
	tracer.state().params.bounces = 2;
	tracer.state().exposure = -1.0f;
	if (!tracer.start(sceneGraph, &cam)) {
		return -1;
	}
	tracer.update();
	const image::ImagePtr img = tracer.image();
	int l = -1;
	if (img) {
		const color::RGBA c = img->colorAt(img->width() / 2, img->height() / 2);
		l = ((int)c.r + (int)c.g + (int)c.b) / 3;
	}
	tracer.stop();
	return l;
}

TEST_F(PathTracerTest, testVoxelDDAMediaDensityAttenuates) {
	const core::String hdriPath = writeSpotHdri(_testApp->filesystem(), "pathtracer_media.hdr", 0, 0);
	ASSERT_FALSE(hdriPath.empty());

	const int openL = mediaFloaterGroundLuma(hdriPath, false, 0.3f, 0.0f);
	const int thinL = mediaFloaterGroundLuma(hdriPath, true, 0.2f, 0.0f);
	const int thickL = mediaFloaterGroundLuma(hdriPath, true, 1.0f, 0.0f);
	ASSERT_GE(openL, 0);
	ASSERT_GE(thinL, 0);
	ASSERT_GE(thickL, 0);
	EXPECT_GT(openL, thickL) << "open=" << openL << " thick=" << thickL;
	EXPECT_GT(thinL, thickL) << "thin=" << thinL << " thick=" << thickL;
}

static int mediaCloudLuma(const core::String &hdriPath, float phase, float exposure) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 0, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(230, 230, 230, 255));
	pal.setMaterialType(1, palette::MaterialType::Volumetric);
	pal.setDensity(1, 0.55f);
	pal.setRimLight(1, phase);
	pal.setScatter(1, 1.0f);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(0).setProperty(scenegraph::PropHdri, true);
	sceneGraph.node(0).setProperty(scenegraph::PropHdriPath, hdriPath);

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(0.5f, 0.5f, 3.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 48;
	tracer.state().params.batch = 48;
	tracer.state().exposure = exposure;
	if (!tracer.start(sceneGraph, &cam)) {
		return -1;
	}
	tracer.update();
	const image::ImagePtr img = tracer.image();
	int l = -1;
	if (img) {
		const color::RGBA c = img->colorAt(img->width() / 2, img->height() / 2);
		l = ((int)c.r + (int)c.g + (int)c.b) / 3;
	}
	tracer.stop();
	return l;
}

TEST_F(PathTracerTest, testVoxelDDAMediaPhaseForward) {
	using namespace voxelpathtracer::sampling;
	// Forward g must peak toward the incoming light. This is the quantity the
	// 8-bit image used to clip through (both sides 255).
	EXPECT_NEAR(henyeyGreenstein(0.0f, 1.0f), 0.07957747f, 0.0001f);
	EXPECT_GT(henyeyGreenstein(0.85f, 1.0f), henyeyGreenstein(0.0f, 1.0f) * 10.0f);
	EXPECT_GT(henyeyGreenstein(0.85f, 1.0f), henyeyGreenstein(0.85f, 0.0f));

	// Sun behind the camera (-Z). Forward g scatters that light toward the lens.
	// Dim the sun so neither image hits the 8-bit ceiling.
	const core::String hdriPath =
		writeSpotHdri(_testApp->filesystem(), "pathtracer_media_phase.hdr", 12, 8, 180, 128);
	ASSERT_FALSE(hdriPath.empty());

	const int isoL = mediaCloudLuma(hdriPath, 0.0f, -8.0f);
	const int fwdL = mediaCloudLuma(hdriPath, 0.85f, -8.0f);
	ASSERT_GE(isoL, 0);
	ASSERT_GE(fwdL, 0);
	ASSERT_GT(isoL, 0) << "iso is black; phase cannot be measured";
	ASSERT_GT(fwdL, 0) << "forward is black; phase cannot be measured";
	ASSERT_LT(isoL, 250) << "iso clipped; phase cannot be measured";
	ASSERT_LT(fwdL, 250) << "forward clipped; phase cannot be measured";
	EXPECT_GT(fwdL, isoL) << "fwd=" << fwdL << " iso=" << isoL;
}

static color::RGBA studioMediaColor(float density, float scatter, float phase, float emit, bool hideEnv,
									int depth = 1, float exposure = 0.0f) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	const int z1 = glm::max(0, depth - 1);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 0, z1));
	for (int z = 0; z <= z1; ++z) {
		v->setVoxel(0, 0, z, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	}
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(220, 80, 40, 255));
	pal.setMaterialType(1, palette::MaterialType::Volumetric);
	pal.setDensity(1, density);
	pal.setScatter(1, scatter);
	pal.setRimLight(1, phase);
	if (emit > 0.0f) {
		pal.setEmit(1, emit);
	}
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(sceneGraph.root().id())
		.setProperty(scenegraph::PropEnvHidden, hideEnv ? "true" : "false");

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(0.5f, 0.5f, 3.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 32;
	tracer.state().params.batch = 32;
	tracer.state().exposure = exposure;
	if (!tracer.start(sceneGraph, &cam)) {
		return color::RGBA(0, 0, 0, 0);
	}
	tracer.state().params.envhidden = hideEnv;
	tracer.update();
	const image::ImagePtr img = tracer.image();
	color::RGBA c(0, 0, 0, 0);
	if (img) {
		c = img->colorAt(img->width() / 2, img->height() / 2);
	}
	tracer.stop();
	return c;
}

static int studioMediaLuma(float density, float scatter, float phase, float emit, bool hideEnv) {
	const color::RGBA c = studioMediaColor(density, scatter, phase, emit, hideEnv);
	if (c.a == 0 && c.r == 0 && c.g == 0 && c.b == 0) {
		return -1;
	}
	return ((int)c.r + (int)c.g + (int)c.b) / 3;
}

TEST_F(PathTracerTest, testVoxelDDAMediaRimBrightens) {
	const int even = studioMediaLuma(0.40f, 1.0f, 0.0f, 0.0f, false);
	const int rim = studioMediaLuma(0.40f, 1.0f, 1.0f, 0.0f, false);
	ASSERT_GE(even, 0);
	ASSERT_GE(rim, 0);
	EXPECT_GT(rim, even) << "rim=" << rim << " even=" << even;
}

TEST_F(PathTracerTest, testVoxelDDAMediaScatterFillsInFrontOfWall) {
	const color::RGBA smoke = studioMediaColor(0.25f, 0.0f, 0.0f, 0.22f, false, 1, -2.0f);
	const color::RGBA cloud = studioMediaColor(0.25f, 1.0f, 0.0f, 0.22f, false, 1, -2.0f);
	const color::RGBA rim = studioMediaColor(0.25f, 1.0f, 1.0f, 0.22f, false, 1, -2.0f);
	EXPECT_GT((int)cloud.r, (int)smoke.r + 20) << "cloud.r=" << (int)cloud.r << " smoke.r=" << (int)smoke.r;
	EXPECT_GT((int)rim.r, (int)smoke.r + 20) << "rim.r=" << (int)rim.r << " smoke.r=" << (int)smoke.r;
}

static int hdriMediaRed(const core::String &hdriPath, float scatter, float phase) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 0, 0, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(255, 0, 0, 255));
	pal.setMaterialType(1, palette::MaterialType::Volumetric);
	pal.setDensity(1, 0.25f);
	pal.setScatter(1, scatter);
	pal.setRimLight(1, phase);
	pal.setEmit(1, 0.30f);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));
	sceneGraph.node(sceneGraph.root().id()).setProperty(scenegraph::PropHdri, "true");
	sceneGraph.node(sceneGraph.root().id()).setProperty(scenegraph::PropHdriPath, hdriPath);
	sceneGraph.node(sceneGraph.root().id()).setProperty(scenegraph::PropEnvHidden, "true");

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(0.5f, 0.5f, 3.0f));
	cam.lookAt(glm::vec3(0.5f, 0.5f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 24;
	tracer.state().params.batch = 24;
	tracer.state().exposure = -1.0f;
	if (!tracer.start(sceneGraph, &cam)) {
		return -1;
	}
	tracer.state().params.envhidden = true;
	tracer.update();
	const image::ImagePtr img = tracer.image();
	int r = -1;
	if (img) {
		r = (int)img->colorAt(img->width() / 2, img->height() / 2).r;
	}
	tracer.stop();
	return r;
}

TEST_F(PathTracerTest, testVoxelDDAMediaScatterReadsWithHdri) {
	const core::String hdriPath = writeSpotHdri(_testApp->filesystem(), "pathtracer_media_scatter.hdr", 0, 0);
	ASSERT_FALSE(hdriPath.empty());
	const int smoke = hdriMediaRed(hdriPath, 0.0f, 0.0f);
	const int cloud = hdriMediaRed(hdriPath, 1.0f, 0.0f);
	const int rim = hdriMediaRed(hdriPath, 1.0f, 1.0f);
	ASSERT_GE(smoke, 0);
	ASSERT_GE(cloud, 0);
	ASSERT_GE(rim, 0);
	EXPECT_GT(cloud, smoke + 15) << "cloud=" << cloud << " smoke=" << smoke;
	EXPECT_GT(rim, smoke + 15) << "rim=" << rim << " smoke=" << smoke;
}

TEST_F(PathTracerTest, testVoxelDDAMediaDensityMidIsSeeThrough) {
	const int mid = studioMediaLuma(0.50f, 0.20f, 0.0f, 0.0f, false);
	const int thick = studioMediaLuma(1.00f, 0.20f, 0.0f, 0.0f, false);
	ASSERT_GE(mid, 0);
	ASSERT_GE(thick, 0);
	// Mid density must not be a solid brick; thick is darker (more extinct).
	EXPECT_GT(mid, 40);
	EXPECT_GT(mid, thick) << "mid=" << mid << " thick=" << thick;
}

TEST_F(PathTracerTest, testVoxelDDAMediaNeighborsMatch) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 1, 0, 0));
	v->setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	v->setVoxel(1, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, 1));
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(210, 200, 180, 255));
	pal.setMaterialType(1, palette::MaterialType::Volumetric);
	pal.setDensity(1, 0.4f);
	pal.setScatter(1, 0.9f);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(1.0f, 0.5f, 3.0f));
	cam.lookAt(glm::vec3(1.0f, 0.5f, 0.5f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 48;
	tracer.state().params.batch = 48;
	tracer.state().exposure = -1.0f;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	ASSERT_TRUE(tracer.update());
	const image::ImagePtr &img = tracer.image();
	ASSERT_TRUE(img);
	auto luma = [&](int x, int y) {
		const color::RGBA c = img->colorAt(x, y);
		return ((int)c.r + (int)c.g + (int)c.b) / 3;
	};
	const int y = img->height() / 2;
	const int left = luma(img->width() * 2 / 5, y);
	const int right = luma(img->width() * 3 / 5, y);
	EXPECT_NEAR(left, right, 25) << "left=" << left << " right=" << right;
	ASSERT_TRUE(tracer.stop());
}

TEST_F(PathTracerTest, testVoxelDDAMediaSlabHasNoGridWalls) {
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode modelNode(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *v = new voxel::RawVolume(voxel::Region(0, 0, 0, 5, 0, 5));
	for (int z = 0; z <= 5; ++z) {
		for (int x = 0; x <= 5; ++x) {
			v->setVoxel(x, 0, z, voxel::createVoxel(voxel::VoxelType::Generic, 1));
		}
	}
	palette::Palette pal;
	pal.nippon();
	pal.setColor(1, color::RGBA(211, 211, 211, 255));
	pal.setMaterialType(1, palette::MaterialType::Volumetric);
	pal.setDensity(1, 0.18f);
	pal.setScatter(1, 1.0f);
	modelNode.setPalette(pal);
	modelNode.setVolume(v);
	sceneGraph.emplace(core::move(modelNode));

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(3.0f, 10.0f, 3.0f));
	cam.lookAt(glm::vec3(3.0f, 0.0f, 3.0f));
	cam.update(0.0);

	voxelpathtracer::VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 64;
	tracer.state().params.samples = 48;
	tracer.state().params.batch = 48;
	tracer.state().exposure = -1.0f;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));
	ASSERT_TRUE(tracer.update());
	const image::ImagePtr &img = tracer.image();
	ASSERT_TRUE(img);
	auto luma = [&](int x, int y) {
		const color::RGBA c = img->colorAt(x, y);
		return ((int)c.r + (int)c.g + (int)c.b) / 3;
	};
	int maxJump = 0;
	int seen = 0;
	const int x0 = img->width() * 3 / 8;
	const int x1 = img->width() * 5 / 8;
	const int y0 = img->height() * 3 / 8;
	const int y1 = img->height() * 5 / 8;
	for (int y = y0; y <= y1; ++y) {
		for (int x = x0; x < x1; ++x) {
			const int d = luma(x, y) - luma(x + 1, y);
			maxJump = glm::max(maxJump, d < 0 ? -d : d);
			seen = luma(x, y);
		}
	}
	// A homogeneous slab must not show a voxel grid (dark cell walls).
	EXPECT_LT(maxJump, 35) << "maxJump=" << maxJump;
	EXPECT_GT(seen, 10);
	ASSERT_TRUE(tracer.stop());
}

TEST_F(PathTracerTest, testTonemapContract) {
	// sRGB encode is the exact inverse of color::srgbToLinear used on input.
	EXPECT_NEAR(voxelpathtracer::pathTracerSrgbEncode(0.0f), 0.0f, 1e-6f);
	EXPECT_NEAR(voxelpathtracer::pathTracerSrgbEncode(0.5f), 0.7354f, 1e-3f);
	EXPECT_NEAR(voxelpathtracer::pathTracerSrgbEncode(1.0f), 1.0f, 1e-6f);

	// No exposure, no filmic == pure sRGB encode (no double/missing gamma).
	const glm::vec3 mid = voxelpathtracer::pathTracerTonemap(glm::vec3(0.5f), 0.0f, false);
	EXPECT_NEAR(mid.x, 0.7354f, 1e-3f);

	// Exposure is base-2 stops applied to linear radiance before sRGB.
	const float plusOne = voxelpathtracer::pathTracerTonemap(glm::vec3(0.25f), 1.0f, false).x;
	const float zeroStop = voxelpathtracer::pathTracerTonemap(glm::vec3(0.25f), 0.0f, false).x;
	EXPECT_NEAR(plusOne, voxelpathtracer::pathTracerSrgbEncode(0.5f), 1e-4f);
	EXPECT_GT(plusOne, zeroStop);

	// Filmic rolls off highlights instead of clipping: a 4x-over-bright value
	// stays below 1.0 with filmic on and exceeds 1.0 (would clip to white) off.
	const float filmicOn = voxelpathtracer::pathTracerTonemap(glm::vec3(4.0f), 0.0f, true).x;
	const float filmicOff = voxelpathtracer::pathTracerTonemap(glm::vec3(4.0f), 0.0f, false).x;
	EXPECT_LT(filmicOn, 1.0f);
	EXPECT_GT(filmicOff, 1.0f);

	// Filmic rolloff is monotonic (no highlight inversion).
	EXPECT_GT(voxelpathtracer::pathTracerFilmic(glm::vec3(4.0f)).x,
			  voxelpathtracer::pathTracerFilmic(glm::vec3(2.0f)).x);
}
