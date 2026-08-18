/**
 * @file
 * Generates the material test scene (material-test.vengi): a grid of voxel
 * clumps, one per material type, lit by an EXR HDRI environment. Each clump is
 * a distinct model node with its own palette carrying the material.
 */

#include "voxelformat/private/vengi/VENGIFormat.h"
#include "AbstractFormatTest.h"
#include "palette/Palette.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "scenegraph/SceneGraphNodeProperties.h"
#include "scenegraph/SceneGraphTransform.h"
#include "voxel/RawVolume.h"
#include "voxel/Region.h"
#include "voxel/Voxel.h"

namespace voxelformat {

namespace {

struct ClumpSpec {
	const char *name;
	color::RGBA color;
	palette::MaterialType type;
	float metal = -1.0f;
	float roughness = -1.0f;
	float specular = -1.0f;
	float ior = -1.0f;
	float attenuation = -1.0f;
	float emit = -1.0f;
	float emitBoost = -1.0f;
	float density = -1.0f;
	float scatter = -1.0f;
	float rimLight = -1.0f;
};

void setIfSet(palette::Palette &pal, uint8_t idx, float value, void (*setter)(palette::Palette &, uint8_t, float)) {
	if (value >= 0.0f) {
		setter(pal, idx, value);
	}
}

// clang-format off
void setMetal(palette::Palette &p, uint8_t i, float v) { p.setMetal(i, v); }
void setRoughness(palette::Palette &p, uint8_t i, float v) { p.setRoughness(i, v); }
void setSpecular(palette::Palette &p, uint8_t i, float v) { p.setSpecular(i, v); }
void setIor(palette::Palette &p, uint8_t i, float v) { p.setIndexOfRefraction(i, v); }
void setAttenuation(palette::Palette &p, uint8_t i, float v) { p.setAttenuation(i, v); }
void setEmit(palette::Palette &p, uint8_t i, float v) { p.setEmit(i, v); }
void setEmitBoost(palette::Palette &p, uint8_t i, float v) { p.setEmitBoost(i, v); }
void setDensity(palette::Palette &p, uint8_t i, float v) { p.setDensity(i, v); }
void setScatter(palette::Palette &p, uint8_t i, float v) { p.setScatter(i, v); }
void setRimLight(palette::Palette &p, uint8_t i, float v) { p.setRimLight(i, v); }
// clang-format on

palette::Palette buildPalette(const ClumpSpec &spec) {
	palette::Palette pal;
	pal.setSize(2);
	pal.setColor(0, color::RGBA(0, 0, 0, 0));
	pal.setColor(1, spec.color);
	pal.setMaterialType(1, spec.type);
	setIfSet(pal, 1, spec.metal, setMetal);
	setIfSet(pal, 1, spec.roughness, setRoughness);
	setIfSet(pal, 1, spec.specular, setSpecular);
	setIfSet(pal, 1, spec.ior, setIor);
	setIfSet(pal, 1, spec.attenuation, setAttenuation);
	setIfSet(pal, 1, spec.emit, setEmit);
	setIfSet(pal, 1, spec.emitBoost, setEmitBoost);
	setIfSet(pal, 1, spec.density, setDensity);
	setIfSet(pal, 1, spec.scatter, setScatter);
	setIfSet(pal, 1, spec.rimLight, setRimLight);
	return pal;
}

void addClump(scenegraph::SceneGraph &sceneGraph, const ClumpSpec &spec, const glm::vec3 &position, bool asBox) {
	palette::Palette pal = buildPalette(spec);
	const int size = 20;
	voxel::RawVolume *vol = new voxel::RawVolume(voxel::Region(0, size - 1));
	const int radius = 8;
	const glm::vec3 center((float)(size / 2));
	const voxel::Region region = vol->region();
	if (asBox) {
		for (int z = region.getLowerZ(); z <= region.getUpperZ(); ++z) {
			for (int y = region.getLowerY(); y <= region.getUpperY(); ++y) {
				for (int x = region.getLowerX(); x <= region.getUpperX(); ++x) {
					vol->setVoxel(x, y, z, voxel::createVoxel(pal, 1));
				}
			}
		}
	} else {
		for (int z = region.getLowerZ(); z <= region.getUpperZ(); ++z) {
			for (int y = region.getLowerY(); y <= region.getUpperY(); ++y) {
				for (int x = region.getLowerX(); x <= region.getUpperX(); ++x) {
					if (glm::length(glm::vec3((float)x, (float)y, (float)z) - center) <= (float)radius) {
						vol->setVoxel(x, y, z, voxel::createVoxel(pal, 1));
					}
				}
			}
		}
	}
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	node.setName(spec.name);
	node.setVolume(vol);
	node.setPalette(pal);
	node.transform(0).setLocalTranslation(position);
	sceneGraph.emplace(core::move(node));
}

} // namespace

class MaterialTestSceneGenerator : public AbstractFormatTest {};

TEST_F(MaterialTestSceneGenerator, generateMaterialTestScene) {
	scenegraph::SceneGraph sceneGraph;

	// Material clumps laid out in two rows of five, sitting on the ground plane.
	const ClumpSpec clumps[] = {
		{"diffuse", color::RGBA(200, 60, 40), palette::MaterialType::Diffuse},
		{"metal-mirror", color::RGBA(220, 220, 225), palette::MaterialType::Metal, 1.0f, 0.05f, 1.0f},
		{"metal-brushed", color::RGBA(200, 200, 210), palette::MaterialType::Metal, 1.0f, 0.5f, 0.6f},
		{"glass-clear", color::RGBA(255, 255, 255), palette::MaterialType::Glass, -1.0f, -1.0f, -1.0f, 1.5f},
		{"glass-tinted", color::RGBA(120, 200, 210), palette::MaterialType::Glass, -1.0f, -1.0f, -1.0f, 1.5f, 0.4f},
		{"emit", color::RGBA(255, 240, 200), palette::MaterialType::Emit, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 1.0f, 1.0f},
		{"alpha", color::RGBA(100, 150, 255, 128), palette::MaterialType::Blend},
		{"volumetric-fog", color::RGBA(230, 235, 240), palette::MaterialType::Volumetric, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.1f, 0.8f, 0.0f},
		{"volumetric-cloud", color::RGBA(250, 250, 255), palette::MaterialType::Volumetric, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.3f, 1.0f, 0.3f},
		{"volumetric-smoke", color::RGBA(80, 80, 85), palette::MaterialType::Volumetric, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.8f, 0.15f, 0.6f},
	};
	const int clumpCount = (int)(sizeof(clumps) / sizeof(clumps[0]));
	const float spacing = 24.0f;
	const float yBase = -2.0f; // sphere bottom (local y=2) rests on the ground plane (top y=0)

	for (int i = 0; i < clumpCount; ++i) {
		const int row = i / 5;
		const int col = i % 5;
		const glm::vec3 position((float)(col - 2) * spacing, yBase, (float)row * spacing);
		const bool asBox = clumps[i].type == palette::MaterialType::Emit;
		addClump(sceneGraph, clumps[i], position, asBox);
	}

	// Ground plane: a wide diffuse slab under the clumps to catch shadows and reflections.
	{
		const ClumpSpec ground = {"ground", color::RGBA(160, 160, 165), palette::MaterialType::Diffuse};
		palette::Palette pal = buildPalette(ground);
		voxel::RawVolume *vol = new voxel::RawVolume(voxel::Region(glm::ivec3(-60, -8, -40), glm::ivec3(60, 0, 40)));
		const voxel::Region region = vol->region();
		for (int z = region.getLowerZ(); z <= region.getUpperZ(); ++z) {
			for (int y = region.getLowerY(); y <= region.getUpperY(); ++y) {
				for (int x = region.getLowerX(); x <= region.getUpperX(); ++x) {
					vol->setVoxel(x, y, z, voxel::createVoxel(pal, 1));
				}
			}
		}
		scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
		node.setName(ground.name);
		node.setVolume(vol);
		node.setPalette(pal);
		node.transform(0).setLocalTranslation(glm::vec3(0.0f));
		sceneGraph.emplace(core::move(node));
	}

	// Root HDRI environment: a garage HDRI provides the ambient light and the
	// reflections the metal/glass clumps need. The basename is resolved next to
	// the scene file on desktop and via the upload filesystem in the browser.
	scenegraph::SceneGraphNode &root = sceneGraph.node(sceneGraph.root().id());
	root.setProperty(scenegraph::PropHdri, "true");
	root.setProperty(scenegraph::PropHdriPath, "abandoned_garage_2k.exr");
	root.setProperty(scenegraph::PropHdriIntensity, "1.0");
	root.setProperty(scenegraph::PropGroundPlane, "false");

	VENGIFormat format;
	const io::ArchivePtr &archive = helper_filesystemarchive();
	ASSERT_TRUE(format.saveGroups(sceneGraph, "material-test.vengi", archive, testSaveCtx))
		<< "Could not save material-test.vengi";
}

} // namespace voxelformat
