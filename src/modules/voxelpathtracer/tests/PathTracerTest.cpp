/**
 * @file
 */

#include "voxelpathtracer/PathTracer.h"
#include "voxelpathtracer/PathTracerState.h"
#include "app/App.h"
#include "app/tests/AbstractTest.h"
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

TEST_F(PathTracerTest, testAppearancePersistsOnScene) {
	scenegraph::SceneGraph sceneGraph;
	voxelpathtracer::PathTracer pathTracer;
	pathTracer.state().hdriEnvironment = true;
	pathTracer.state().hdriPath = "studio.hdr";
	pathTracer.state().hdriIntensity = 2.5f;
	pathTracer.state().hdriAzimuth = glm::radians(45.0f);
	pathTracer.state().groundPlane = true;
	pathTracer.state().studioEdges = true;
	pathTracer.writeAppearanceToScene(sceneGraph);

	const scenegraph::SceneGraphNode &root = sceneGraph.root();
	EXPECT_EQ(root.property(scenegraph::PropHdri), "true");
	EXPECT_EQ(root.property(scenegraph::PropHdriPath), "studio.hdr");
	EXPECT_EQ(root.property(scenegraph::PropGroundPlane), "true");
	EXPECT_EQ(root.property(scenegraph::PropStudioEdges), "true");

	pathTracer.state().resetAppearance();
	ASSERT_FALSE(pathTracer.state().hdriEnvironment);
	pathTracer.applyAppearanceFromScene(sceneGraph);
	EXPECT_TRUE(pathTracer.state().hdriEnvironment);
	EXPECT_EQ(pathTracer.state().hdriPath, "studio.hdr");
	EXPECT_NEAR(pathTracer.state().hdriIntensity, 2.5f, 0.01f);
	EXPECT_NEAR(pathTracer.state().hdriAzimuth, glm::radians(45.0f), 0.01f);
	EXPECT_TRUE(pathTracer.state().groundPlane);
	EXPECT_TRUE(pathTracer.state().studioEdges);
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

TEST_F(PathTracerTest, testGroundPlaneAddsShape) {
	scenegraph::SceneGraph sceneGraph;
	addUnitCube(sceneGraph);
	sceneGraph.node(0).setProperty(scenegraph::PropGroundPlane, true);
	video::Camera cam = testCamera();
	voxelpathtracer::PathTracer pathTracer;
	ASSERT_TRUE(pathTracer.start(sceneGraph, &cam));
	int triCount = 0;
	for (const yocto::shape_data &shape : pathTracer.state().scene.shapes) {
		triCount += (int)shape.triangles.size();
	}
	// One cube is 12 tris; the ground plane adds 2.
	EXPECT_GE(triCount, 14);
	ASSERT_TRUE(pathTracer.stop());
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
