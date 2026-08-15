/**
 * @file
 */

#include "voxelpathtracer/PathTracer.h"
#include "voxelpathtracer/PathTracerState.h"
#include "app/App.h"
#include "app/tests/AbstractTest.h"
#include "image/Image.h"
#include "io/FileStream.h"
#include "io/FilesystemArchive.h"
#include "io/FormatDescription.h"
#include "scenegraph/SceneGraph.h"
#include "voxelformat/FormatConfig.h"
#include "voxelformat/VolumeFormat.h"
#include "core/ConfigVar.h"
#include "core/GLM.h"
#include "core/Var.h"
#include "video/Camera.h"
#include "voxel/SurfaceExtractor.h"

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
	EXPECT_TRUE(image::writePNG(img, stream));
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
