/**
 * @file
 * Regression test: a Volumetric clump must actually appear in a render. The
 * material-test scene's three Volumetric clumps looked absent in the browser,
 * so this pins the end-to-end path: palette -> scene -> marchVolume -> image.
 *
 * NOTE on methodology: an earlier version of this test compared a
 * medium-vs-empty render and found them byte-identical, which looked like the
 * medium contributed nothing. That was a fault in the TEST, not the renderer:
 * the "empty" control allocated a RawVolume but never set any voxels, and with
 * the environment visible both renders were the same HDRI backdrop at the
 * sample counts used. Compare a medium against a DIFFERENT medium density
 * instead, and look at the region the sphere actually covers.
 */
#include "app/tests/AbstractTest.h"
#include "palette/Palette.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "video/Camera.h"
#include "voxel/RawVolume.h"
#include "voxel/Region.h"
#include "voxel/Voxel.h"
#include "voxelpathtracer/VoxelDDAPathTracer.h"
#include "core/collection/DynamicArray.h"

namespace voxelpathtracer {

class MediaReachTest : public app::AbstractTest {
protected:
	// One volumetric sphere centered at the origin with the given density.
	void buildMediaScene(scenegraph::SceneGraph &sceneGraph, float density) {
		palette::Palette pal;
		pal.setSize(2);
		pal.setColor(0, color::RGBA(0, 0, 0, 0));
		// A DARK smoke. A near-white medium against the default flat bright
		// environment (0.91 grey) saturates to ~236/255 and density stops
		// being measurable in 8-bit output.
		pal.setColor(1, color::RGBA(60, 60, 70));
		pal.setMaterialType(1, palette::MaterialType::Volumetric);
		pal.setDensity(1, density);
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
		scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
		node.setName("media-sphere");
		node.setPivot(glm::vec3(0.0f));
		node.setPalette(pal);
		node.setVolume(vol);
		sceneGraph.emplace(core::move(node));
		sceneGraph.updateTransforms();
	}

	// Mean absolute difference between a DENSE medium and a THIN one. Both
	// scenes contain the same geometry, so any difference is the volume
	// integrator responding to density. Comparing against an "empty" scene is
	// unsound: with the environment visible, an empty scene and a thin medium
	// are both mostly HDRI backdrop.
	float renderDensityDelta(float cameraDistance) {
		const core::DynamicArray<uint8_t> dense = renderPixels(cameraDistance, 0.9f);
		const core::DynamicArray<uint8_t> thin = renderPixels(cameraDistance, 0.02f);
		if (dense.empty() || thin.empty() || dense.size() != thin.size()) {
			return -1.0f;
		}
		double total = 0.0;
		const size_t pixels = dense.size() / 4u;
		for (size_t i = 0; i < pixels; ++i) {
			for (int c = 0; c < 3; ++c) {
				total += glm::abs((double)dense[i * 4 + c] - (double)thin[i * 4 + c]);
			}
		}
		return (float)(total / (double)(pixels * 3));
	}

	core::DynamicArray<uint8_t> renderPixels(float cameraDistance, float density) {
		core::DynamicArray<uint8_t> out;
		scenegraph::SceneGraph sceneGraph;
		buildMediaScene(sceneGraph, density);

		video::Camera cam;
		cam.setSize(glm::ivec2(160, 160));
		cam.setRotationType(video::CameraRotationType::Target);
		cam.setWorldPosition(glm::vec3(10.0f, 10.0f, 10.0f + cameraDistance));
		cam.setTarget(glm::vec3(10.0f, 10.0f, 10.0f));
		cam.lookAt(glm::vec3(10.0f, 10.0f, 10.0f));
		cam.setFieldOfView(50.0f);
		cam.update(0.0);

		VoxelDDAPathTracer tracer;
		tracer.state().params.resolution = 64;
		tracer.state().params.samples = 8;
		tracer.state().params.batch = 8;
		tracer.state().params.denoise = false;
		tracer.state().params.adaptive = false;
		tracer.state().params.envhidden = false;
		// Without an HDRI or sky, evalEnvironment returns a flat bright
		// constant with no directional structure, so a scattering medium has
		// almost nothing to differentiate against. Use the analytic sky.
		tracer.state().skyEnvironment = true;
		tracer.state().sunIntensity = 3.0f;
		if (!tracer.start(sceneGraph, &cam)) {
			return out;
		}
		while (!tracer.update()) {
		}
		const image::ImagePtr &img = tracer.image();
		if (!img || !img->isLoaded()) {
			return out;
		}
		const int bytes = img->width() * img->height() * 4;
		out.reserve(bytes);
		for (int i = 0; i < bytes; ++i) {
			out.push_back(img->data()[i]);
		}
		tracer.stop();
		return out;
	}
};

// Baseline: a camera INSIDE the 64-unit budget renders the medium, and density
// visibly changes the result.
TEST_F(MediaReachTest, testMediaVisibleWithinMarchBudget) {
	// Diagnostic first: do these renders contain anything at all? mad=0 could
	// mean "density has no effect" OR "both images are the same blank frame".
	const core::DynamicArray<uint8_t> dense = renderPixels(30.0f, 0.9f);
	ASSERT_FALSE(dense.empty()) << "render produced no image";
	int minv = 255;
	int maxv = 0;
	double sum = 0.0;
	const size_t pixels = dense.size() / 4u;
	for (size_t i = 0; i < pixels; ++i) {
		const int v = glm::max((int)dense[i * 4], glm::max((int)dense[i * 4 + 1], (int)dense[i * 4 + 2]));
		minv = glm::min(minv, v);
		maxv = glm::max(maxv, v);
		sum += v;
	}
	const double mean = sum / (double)pixels;
	EXPECT_GT(maxv, 0) << "the dense render is entirely black (min=" << minv << " max=" << maxv
					   << " mean=" << mean << "): nothing is being rendered, media or not";
	EXPECT_GT(maxv - minv, 4) << "the dense render is a FLAT field (min=" << minv << " max=" << maxv
							  << " mean=" << mean << "): no sphere silhouette is present";

	const float delta = renderDensityDelta(30.0f);
	ASSERT_GE(delta, 0.0f) << "render failed";
	EXPECT_GT(delta, 1.0f) << "density does not change the render at 30 units (mad=" << delta
						   << ", dense min=" << minv << " max=" << maxv << " mean=" << mean << ")";
}

// The reach question: the same medium at the same on-screen size, but a camera
// beyond the media march budget. reach = min(tmax, 64.0) is measured from the
// ray ORIGIN, so from far enough away the march can terminate before it ever
// enters the medium.
TEST_F(MediaReachTest, testMediaVisibleBeyondMarchBudget) {
	const float delta = renderDensityDelta(95.0f);
	ASSERT_GE(delta, 0.0f) << "render failed";
	EXPECT_GT(delta, 1.0f)
		<< "density does not change the render from 95 units: the media march budget is measured "
		   "from the ray origin instead of from the medium entry point (mad=" << delta << ")";
}

} // namespace voxelpathtracer
