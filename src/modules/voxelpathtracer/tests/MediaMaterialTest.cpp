/**
 * @file
 * Diagnostic: does a Volumetric palette entry reach the path tracer scene as a
 * media material at all? Checks the palette -> PathTracerScene conversion
 * directly, before any ray marching, so a failure here means the bug is in the
 * material classification rather than in the volume integrator.
 */
#include "app/tests/AbstractTest.h"
#include "palette/Palette.h"
#include "palette/Material.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "video/Camera.h"
#include "voxel/RawVolume.h"
#include "voxel/Region.h"
#include "voxel/Voxel.h"
#include "voxelpathtracer/PathTracerMaterial.h"
#include "voxelpathtracer/VoxelDDAPathTracer.h"

namespace voxelpathtracer {

class MediaMaterialTest : public app::AbstractTest {};

TEST_F(MediaMaterialTest, testVolumetricPaletteBecomesMediaMaterial) {
	palette::Palette pal;
	pal.setSize(2);
	pal.setColor(0, color::RGBA(0, 0, 0, 0));
	pal.setColor(1, color::RGBA(250, 250, 255));
	pal.setMaterialType(1, palette::MaterialType::Volumetric);
	pal.setDensity(1, 0.9f);
	pal.setScatter(1, 1.0f);

	// Read the values straight back out of the palette first: if these are
	// wrong the scene build cannot possibly be right.
	const palette::Material &mat = pal.material(1);
	EXPECT_EQ(mat.type, palette::MaterialType::Volumetric) << "palette lost the Volumetric type";
	EXPECT_TRUE(mat.has(palette::MaterialProperty::MaterialDensity)) << "palette lost density";
	EXPECT_NEAR(mat.value(palette::MaterialProperty::MaterialDensity), 0.9f, 0.01f);
	EXPECT_TRUE(mat.has(palette::MaterialProperty::MaterialScatter)) << "palette lost scatter";
	EXPECT_NEAR(mat.value(palette::MaterialProperty::MaterialScatter), 1.0f, 0.01f);

	const int size = 20;
	voxel::RawVolume *vol = new voxel::RawVolume(voxel::Region(0, size - 1));
	const glm::vec3 center((float)(size / 2));
	for (int z = 0; z < size; ++z) {
		for (int y = 0; y < size; ++y) {
			for (int x = 0; x < size; ++x) {
				if (glm::length(glm::vec3((float)x, (float)y, (float)z) - center) <= 8.0f) {
					vol->setVoxel(x, y, z, voxel::createVoxel(pal, 1));
				}
			}
		}
	}
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	node.setName("media-sphere");
	node.setPivot(glm::vec3(0.0f));
	node.setPalette(pal);
	node.setVolume(vol);
	sceneGraph.emplace(core::move(node));
	sceneGraph.updateTransforms();

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setRotationType(video::CameraRotationType::Target);
	cam.setWorldPosition(glm::vec3(10.0f, 10.0f, 40.0f));
	cam.setTarget(glm::vec3(10.0f, 10.0f, 10.0f));
	cam.lookAt(glm::vec3(10.0f, 10.0f, 10.0f));
	cam.update(0.0);

	VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 32;
	tracer.state().params.samples = 1;
	tracer.state().params.batch = 1;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));

	// hasMedia() must be true, otherwise every media march early-returns.
	EXPECT_TRUE(tracer.hasMediaForTest())
		<< "the tracer did not classify any material as media: fieldMediaT/marchVolume early-return "
		   "and volumetric voxels contribute nothing";

	// Classification is not enough: the marcher finds media via a world->grid
	// lookup. Probe the sphere center, which is solidly inside the volume.
	float density = -1.0f;
	float scatter = -1.0f;
	float rimLight = -1.0f;
	const bool found = tracer.sampleMediaForTest(glm::vec3(10.0f, 10.0f, 10.0f), density, scatter, rimLight);
	EXPECT_TRUE(found) << "sampleMedia found NO medium at the sphere center (10,10,10): the world->grid "
						  "lookup in sampleMedia does not resolve, so every march step continues past it";
	if (found) {
		EXPECT_NEAR(density, 0.9f, 0.02f) << "density did not survive the palette->scene conversion";
		EXPECT_NEAR(scatter, 1.0f, 0.02f) << "scatter did not survive the palette->scene conversion";
	}

	tracer.stop();
}

// The last link: sampleMedia finds the medium, so does marchVolume actually
// attenuate light and accumulate in-scattered radiance along a ray that passes
// straight through the sphere?
TEST_F(MediaMaterialTest, testMarchVolumeAttenuatesAndScatters) {
	palette::Palette pal;
	pal.setSize(2);
	pal.setColor(0, color::RGBA(0, 0, 0, 0));
	pal.setColor(1, color::RGBA(250, 250, 255));
	pal.setMaterialType(1, palette::MaterialType::Volumetric);
	pal.setDensity(1, 0.9f);
	pal.setScatter(1, 1.0f);

	const int size = 20;
	voxel::RawVolume *vol = new voxel::RawVolume(voxel::Region(0, size - 1));
	const glm::vec3 center((float)(size / 2));
	for (int z = 0; z < size; ++z) {
		for (int y = 0; y < size; ++y) {
			for (int x = 0; x < size; ++x) {
				if (glm::length(glm::vec3((float)x, (float)y, (float)z) - center) <= 8.0f) {
					vol->setVoxel(x, y, z, voxel::createVoxel(pal, 1));
				}
			}
		}
	}
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	node.setName("media-sphere");
	node.setPivot(glm::vec3(0.0f));
	node.setPalette(pal);
	node.setVolume(vol);
	sceneGraph.emplace(core::move(node));
	sceneGraph.updateTransforms();

	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setRotationType(video::CameraRotationType::Target);
	cam.setWorldPosition(glm::vec3(10.0f, 10.0f, 40.0f));
	cam.setTarget(glm::vec3(10.0f, 10.0f, 10.0f));
	cam.lookAt(glm::vec3(10.0f, 10.0f, 10.0f));
	cam.update(0.0);

	VoxelDDAPathTracer tracer;
	tracer.state().params.resolution = 32;
	tracer.state().params.samples = 1;
	tracer.state().params.batch = 1;
	ASSERT_TRUE(tracer.start(sceneGraph, &cam));

	// A ray from +Z straight through the sphere center along -Z.
	glm::vec3 accumulated(0.0f);
	const glm::vec3 transmittance =
		tracer.marchVolumeForTest(glm::vec3(10.0f, 10.0f, 40.0f), glm::vec3(0.0f, 0.0f, -1.0f), 1.0e30f, accumulated);

	// Density 0.9 over ~16 voxels must absorb a lot: transmittance well under 1.
	const float maxT = glm::max(transmittance.x, glm::max(transmittance.y, transmittance.z));
	EXPECT_LT(maxT, 0.9f) << "marchVolume did not attenuate at all (T=" << maxT
						  << "): the medium is transparent to the integrator";

	// Scatter 1.0 under an environment must in-scatter some radiance.
	const float maxColor = glm::max(accumulated.x, glm::max(accumulated.y, accumulated.z));
	EXPECT_GT(maxColor, 1.0e-4f) << "marchVolume accumulated NO radiance (color=" << maxColor
								 << ") even though sampleMedia finds the medium and scatter=1.0";

	tracer.stop();
}

} // namespace voxelpathtracer
