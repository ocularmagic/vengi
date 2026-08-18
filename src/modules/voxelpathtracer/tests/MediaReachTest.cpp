/**
 * @file
 * Regression test: a Volumetric clump must actually appear in a render. The
 * material-test scene's three Volumetric clumps looked absent in the browser,
 * so this pins the end-to-end path: palette -> scene -> marchVolume -> image.
 *
 * Methodology (do not regress to the earlier dead ends):
 * - Compare DENSE vs THIN density of the SAME geometry. An empty control that
 *   never sets voxels is just the HDRI backdrop and produces mad=0 for the
 *   wrong reason.
 * - Load a dim directional HDRI and use a large negative exposure. A flat
 *   0.91 environment or analytic sky saturates 8-bit output (min=245 max=247)
 *   and density becomes unmeasurable.
 * - Measure only the sphere's screen center, not the whole frame.
 */
#include "app/tests/AbstractTest.h"
#include "io/File.h"
#include "io/FileStream.h"
#include "io/Filesystem.h"
#include "palette/Palette.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "scenegraph/SceneGraphNodeProperties.h"
#include "video/Camera.h"
#include "voxel/RawVolume.h"
#include "voxel/Region.h"
#include "voxel/Voxel.h"
#include "voxelpathtracer/VoxelDDAPathTracer.h"
#include "core/collection/DynamicArray.h"

namespace voxelpathtracer {

class MediaReachTest : public app::AbstractTest {
protected:
	core::String writeSpotHdri(const char *name) {
		const int w = 16;
		const int h = 16;
		const core::String path = _testApp->filesystem()->homeWritePath(name);
		io::FilePtr file = core::make_shared<io::File>(path, io::FileMode::SysWrite);
		if (!file->validHandle()) {
			return "";
		}
		io::FileStream stream(file);
		const core::String header =
			core::String::format("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %i +X %i\n", h, w);
		if (stream.write(header.c_str(), (int)header.size()) != (int)header.size()) {
			return "";
		}
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				const bool sun = (x == 8 && y == 4);
				const uint8_t px[4] = {sun ? (uint8_t)180 : (uint8_t)4, sun ? (uint8_t)180 : (uint8_t)4,
									   sun ? (uint8_t)180 : (uint8_t)4, sun ? (uint8_t)128 : (uint8_t)128};
				if (stream.write(px, 4) != 4) {
					return "";
				}
			}
		}
		stream.close();
		return path;
	}

	void buildMediaScene(scenegraph::SceneGraph &sceneGraph, float density, const core::String &hdriPath) {
		palette::Palette pal;
		pal.setSize(2);
		pal.setColor(0, color::RGBA(0, 0, 0, 0));
		// Dark smoke so density stays measurable after filmic + 8-bit output.
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
		sceneGraph.node(sceneGraph.root().id()).setProperty(scenegraph::PropHdri, "true");
		sceneGraph.node(sceneGraph.root().id()).setProperty(scenegraph::PropHdriPath, hdriPath);
		// Hide the backdrop so the assertion measures in-scattered media, not
		// a sky that already looks like a thin mist in 8-bit output.
		sceneGraph.node(sceneGraph.root().id()).setProperty(scenegraph::PropEnvHidden, "true");
		sceneGraph.updateTransforms();
	}

	struct CropStats {
		float mean = 0.0f;
		int minv = 255;
		int maxv = 0;
		bool valid = false;
	};

	CropStats cropStats(const core::DynamicArray<uint8_t> &pixels, int width, int height) {
		CropStats stats;
		if (pixels.empty() || width <= 0 || height <= 0) {
			return stats;
		}
		const int x0 = width / 2 - 4;
		const int y0 = height / 2 - 4;
		const int x1 = x0 + 8;
		const int y1 = y0 + 8;
		double sum = 0.0;
		int count = 0;
		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				const size_t i = ((size_t)y * (size_t)width + (size_t)x) * 4u;
				const int v = glm::max((int)pixels[i], glm::max((int)pixels[i + 1], (int)pixels[i + 2]));
				stats.minv = glm::min(stats.minv, v);
				stats.maxv = glm::max(stats.maxv, v);
				sum += v;
				++count;
			}
		}
		if (count > 0) {
			stats.mean = (float)(sum / (double)count);
			stats.valid = true;
		}
		return stats;
	}

	float cropAlpha(const core::DynamicArray<uint8_t> &pixels, int width, int height) {
		if (pixels.empty() || width <= 0 || height <= 0) {
			return -1.0f;
		}
		const int x0 = width / 2 - 4;
		const int y0 = height / 2 - 4;
		double sum = 0.0;
		int count = 0;
		for (int y = y0; y < y0 + 8; ++y) {
			for (int x = x0; x < x0 + 8; ++x) {
				const size_t i = ((size_t)y * (size_t)width + (size_t)x) * 4u;
				sum += pixels[i + 3];
				++count;
			}
		}
		return count == 0 ? -1.0f : (float)(sum / (double)count);
	}

	float cropDelta(const core::DynamicArray<uint8_t> &dense, const core::DynamicArray<uint8_t> &thin, int width,
					int height) {
		if (dense.empty() || thin.empty() || dense.size() != thin.size()) {
			return -1.0f;
		}
		const int x0 = width / 2 - 4;
		const int y0 = height / 2 - 4;
		const int x1 = x0 + 8;
		const int y1 = y0 + 8;
		double total = 0.0;
		int count = 0;
		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				const size_t i = ((size_t)y * (size_t)width + (size_t)x) * 4u;
				for (int c = 0; c < 3; ++c) {
					total += glm::abs((double)dense[i + c] - (double)thin[i + c]);
					++count;
				}
			}
		}
		return count == 0 ? -1.0f : (float)(total / (double)count);
	}

	core::DynamicArray<uint8_t> renderPixels(float cameraDistance, float density, const core::String &hdriPath) {
		core::DynamicArray<uint8_t> out;
		scenegraph::SceneGraph sceneGraph;
		buildMediaScene(sceneGraph, density, hdriPath);

		video::Camera cam;
		cam.setSize(glm::ivec2(64, 64));
		cam.setWorldPosition(glm::vec3(10.0f, 10.0f, 10.0f + cameraDistance));
		cam.lookAt(glm::vec3(10.0f, 10.0f, 10.0f));
		cam.update(0.0);

		VoxelDDAPathTracer tracer;
		tracer.state().params.resolution = 64;
		tracer.state().params.samples = 8;
		tracer.state().params.batch = 8;
		tracer.state().params.denoise = false;
		tracer.state().params.adaptive = false;
		tracer.state().hdriEnvironment = true;
		tracer.state().hdriPath = hdriPath;
		tracer.state().params.envhidden = true;
		tracer.state().exposure = -2.0f;
		if (!tracer.start(sceneGraph, &cam)) {
			return out;
		}
		if (!tracer.state().hdriEnvironment || !tracer.hasMediaForTest()) {
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

TEST_F(MediaReachTest, testMediaVisibleWithinMarchBudget) {
	const core::String hdriPath = writeSpotHdri("media_reach_near.hdr");
	ASSERT_FALSE(hdriPath.empty());

	const core::DynamicArray<uint8_t> dense = renderPixels(30.0f, 0.9f, hdriPath);
	const core::DynamicArray<uint8_t> thin = renderPixels(30.0f, 0.02f, hdriPath);
	ASSERT_FALSE(dense.empty()) << "render produced no image (HDRI/media start failed)";
	ASSERT_FALSE(thin.empty()) << "thin render produced no image";

	scenegraph::SceneGraph probe;
	buildMediaScene(probe, 0.9f, hdriPath);
	video::Camera cam;
	cam.setSize(glm::ivec2(64, 64));
	cam.setWorldPosition(glm::vec3(10.0f, 10.0f, 40.0f));
	cam.lookAt(glm::vec3(10.0f, 10.0f, 10.0f));
	cam.update(0.0);
	VoxelDDAPathTracer probeTracer;
	probeTracer.state().hdriEnvironment = true;
	probeTracer.state().hdriPath = hdriPath;
	ASSERT_TRUE(probeTracer.start(probe, &cam));
	EXPECT_TRUE(probeTracer.hasMediaForTest());
	EXPECT_TRUE(probeTracer.state().params.envhidden);
	glm::vec3 accumulated(0.0f);
	const glm::vec3 transmittance = probeTracer.marchVolumeForTest(
		glm::vec3(10.0f, 10.0f, 40.0f), glm::vec3(0.0f, 0.0f, -1.0f), 1.0e30f, accumulated);
	const float maxT = glm::max(transmittance.x, glm::max(transmittance.y, transmittance.z));
	const float maxColor = glm::max(accumulated.x, glm::max(accumulated.y, accumulated.z));
	EXPECT_LT(maxT, 0.9f) << "marchVolume did not attenuate on the same scene the image uses (T=" << maxT << ")";
	EXPECT_GT(maxColor, 1.0e-4f) << "marchVolume accumulated no radiance (color=" << maxColor << ")";
	probeTracer.stop();

	const CropStats stats = cropStats(dense, 64, 64);
	ASSERT_TRUE(stats.valid);
	EXPECT_GT(stats.maxv, 0) << "dense center crop is black (min=" << stats.minv << " max=" << stats.maxv
							 << " mean=" << stats.mean << ")";
	EXPECT_LT(stats.maxv, 250) << "dense center crop is clipped (min=" << stats.minv << " max=" << stats.maxv
							   << " mean=" << stats.mean << "): density cannot be measured";

	const float denseA = cropAlpha(dense, 64, 64);
	const float thinA = cropAlpha(thin, 64, 64);
	EXPECT_GT(denseA, thinA + 4.0f) << "density does not change center-crop alpha at 30 units (denseA=" << denseA
									<< " thinA=" << thinA << ", rgb mad=" << cropDelta(dense, thin, 64, 64)
									<< ", dense min=" << stats.minv << " max=" << stats.maxv << " mean=" << stats.mean
									<< ")";
}

TEST_F(MediaReachTest, testMediaVisibleBeyondMarchBudget) {
	const core::String hdriPath = writeSpotHdri("media_reach_far.hdr");
	ASSERT_FALSE(hdriPath.empty());

	const core::DynamicArray<uint8_t> dense = renderPixels(95.0f, 0.9f, hdriPath);
	const core::DynamicArray<uint8_t> thin = renderPixels(95.0f, 0.02f, hdriPath);
	ASSERT_FALSE(dense.empty()) << "render produced no image";
	ASSERT_FALSE(thin.empty()) << "thin render produced no image";

	const CropStats stats = cropStats(dense, 64, 64);
	ASSERT_TRUE(stats.valid);
	const float denseA = cropAlpha(dense, 64, 64);
	const float thinA = cropAlpha(thin, 64, 64);
	EXPECT_GT(denseA, thinA + 4.0f)
		<< "density does not change center-crop alpha from 95 units: the media march budget is measured "
		   "from the ray origin instead of from the medium entry point (denseA="
		<< denseA << " thinA=" << thinA << ", rgb mad=" << cropDelta(dense, thin, 64, 64) << ", dense min=" << stats.minv
		<< " max=" << stats.maxv << " mean=" << stats.mean << ")";
}

} // namespace voxelpathtracer
