/**
 * @file
 * Generates the material test scene (material-test.vengi): a grid of voxel
 * clumps, one per material type, lit by an EXR HDRI environment. Each clump is
 * a distinct model node. Every node carries the same complete named palette so
 * all test materials remain visible and manually assignable in voxedit.
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

void applyMaterial(palette::Palette &pal, uint8_t idx, const ClumpSpec &spec) {
	pal.setColor(idx, spec.color);
	pal.setColorName(idx, spec.name);
	pal.setMaterialType(idx, spec.type);
	setIfSet(pal, idx, spec.metal, setMetal);
	setIfSet(pal, idx, spec.roughness, setRoughness);
	setIfSet(pal, idx, spec.specular, setSpecular);
	setIfSet(pal, idx, spec.ior, setIor);
	setIfSet(pal, idx, spec.attenuation, setAttenuation);
	setIfSet(pal, idx, spec.emit, setEmit);
	setIfSet(pal, idx, spec.emitBoost, setEmitBoost);
	setIfSet(pal, idx, spec.density, setDensity);
	setIfSet(pal, idx, spec.scatter, setScatter);
	setIfSet(pal, idx, spec.rimLight, setRimLight);
}

void addClump(scenegraph::SceneGraph &sceneGraph, const palette::Palette &pal, uint8_t materialIndex,
			  const ClumpSpec &spec, const glm::vec3 &position, bool asBox) {
	const int size = 20;
	voxel::RawVolume *vol = new voxel::RawVolume(voxel::Region(0, size - 1));
	const int radius = 8;
	const glm::vec3 center((float)(size / 2));
	const voxel::Region region = vol->region();
	if (asBox) {
		// Thin panel: full in X/Z, a thin slice in Y (a light fixture).
		for (int z = region.getLowerZ(); z <= region.getUpperZ(); ++z) {
			for (int y = region.getLowerY(); y <= region.getUpperY(); ++y) {
				for (int x = region.getLowerX(); x <= region.getUpperX(); ++x) {
					if (y >= 8 && y <= 11) {
						vol->setVoxel(x, y, z, voxel::createVoxel(pal, materialIndex));
					}
				}
			}
		}
	} else {
		for (int z = region.getLowerZ(); z <= region.getUpperZ(); ++z) {
			for (int y = region.getLowerY(); y <= region.getUpperY(); ++y) {
				for (int x = region.getLowerX(); x <= region.getUpperX(); ++x) {
					if (glm::length(glm::vec3((float)x, (float)y, (float)z) - center) <= (float)radius) {
						vol->setVoxel(x, y, z, voxel::createVoxel(pal, materialIndex));
					}
				}
			}
		}
	}
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	node.setName(spec.name);
	node.setPivot(glm::vec3(0.0f));
	node.transform(0).setLocalTranslation(position);
	node.setPalette(pal);
	node.setVolume(vol);
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
		{"emit", color::RGBA(255, 240, 200), palette::MaterialType::Emit, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.4f, -1.0f},
		{"alpha", color::RGBA(100, 150, 255, 128), palette::MaterialType::Blend},
		{"volumetric-fog", color::RGBA(230, 235, 240), palette::MaterialType::Volumetric, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.1f, 0.8f, 0.0f},
		{"volumetric-cloud", color::RGBA(250, 250, 255), palette::MaterialType::Volumetric, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.3f, 1.0f, 0.3f},
		{"volumetric-smoke", color::RGBA(80, 80, 85), palette::MaterialType::Volumetric, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.8f, 0.15f, 0.6f},
	};
	const int clumpCount = (int)(sizeof(clumps) / sizeof(clumps[0]));
	const ClumpSpec ground = {"ground", color::RGBA(100, 105, 110), palette::MaterialType::Diffuse};
	palette::Palette materialPalette;
	materialPalette.setSize(clumpCount + 2);
	materialPalette.setColor(0, color::RGBA(0, 0, 0, 0));
	materialPalette.setColorName(0, "empty");
	for (int i = 0; i < clumpCount; ++i) {
		applyMaterial(materialPalette, (uint8_t)(i + 1), clumps[i]);
	}
	applyMaterial(materialPalette, (uint8_t)(clumpCount + 1), ground);
	const float spacing = 24.0f;
	const float yBase = -2.0f; // sphere bottom (local y=2) rests on the ground plane (top y=0)

	for (int i = 0; i < clumpCount; ++i) {
		const int row = i / 5;
		const int col = i % 5;
		const glm::vec3 position((float)(col - 2) * spacing, yBase, (float)row * spacing);
		const bool asBox = clumps[i].type == palette::MaterialType::Emit;
		addClump(sceneGraph, materialPalette, (uint8_t)(i + 1), clumps[i], position, asBox);
	}

	// Ground plane: a wide diffuse slab under the clumps to catch shadows and reflections.
	{
		// Keep the receiver mid-gray so it shows shadows without dominating the
		// filmic output or hiding clear glass against a clipped white slab.
		voxel::RawVolume *vol = new voxel::RawVolume(voxel::Region(glm::ivec3(-60, -8, -40), glm::ivec3(60, 0, 40)));
		const voxel::Region region = vol->region();
		for (int z = region.getLowerZ(); z <= region.getUpperZ(); ++z) {
			for (int y = region.getLowerY(); y <= region.getUpperY(); ++y) {
				for (int x = region.getLowerX(); x <= region.getUpperX(); ++x) {
					vol->setVoxel(x, y, z,
						voxel::createVoxel(materialPalette, (uint8_t)(clumpCount + 1)));
				}
			}
		}
		scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
		node.setName(ground.name);
		node.setPivot(glm::vec3(0.0f));
		node.transform(0).setLocalTranslation(glm::vec3(0.0f));
		node.setPalette(materialPalette);
		node.setVolume(vol);
		sceneGraph.emplace(core::move(node));
	}

	// Propagate the local translations into world transforms before saving.
	sceneGraph.updateTransforms();

	// Every model uses the same complete palette. This keeps every test
	// material visible and manually assignable in the Palette panel, regardless
	// of which scene node is active.
	const int expectedPaletteSize = clumpCount + 2; // transparent + clumps + ground
	for (int i = 0; i < clumpCount; ++i) {
		const scenegraph::SceneGraphNode *found = nullptr;
		for (auto iter = sceneGraph.beginModel(); iter != sceneGraph.end(); ++iter) {
			if ((*iter).name() == clumps[i].name) {
				found = &*iter;
				break;
			}
		}
		ASSERT_NE(nullptr, found) << "Missing material-test node " << clumps[i].name;
		const uint8_t materialIndex = (uint8_t)(i + 1);
		EXPECT_EQ(expectedPaletteSize, found->palette().size());
		EXPECT_EQ(clumps[i].name, found->palette().colorName(materialIndex));
		EXPECT_EQ(clumps[i].type, found->palette().material(materialIndex).type);
		EXPECT_EQ(materialIndex, found->volume()->voxel(10, 10, 10).getColor());
	}

	const scenegraph::SceneGraphNode *groundNode = nullptr;
	for (auto iter = sceneGraph.beginModel(); iter != sceneGraph.end(); ++iter) {
		if ((*iter).name() == "ground") {
			groundNode = &*iter;
			break;
		}
	}
	ASSERT_NE(nullptr, groundNode);
	EXPECT_EQ(expectedPaletteSize, groundNode->palette().size());
	EXPECT_EQ("ground", groundNode->palette().colorName((uint8_t)(clumpCount + 1)));
	EXPECT_EQ((uint8_t)(clumpCount + 1), groundNode->volume()->voxel(0, 0, 0).getColor());

	// Root HDRI environment: a garage HDRI provides the ambient light and the
	// reflections the metal/glass clumps need. The basename is resolved next to
	// the scene file on desktop and via the upload filesystem in the browser.
	scenegraph::SceneGraphNode &root = sceneGraph.node(sceneGraph.root().id());
	root.setProperty(scenegraph::PropHdri, "true");
	root.setProperty(scenegraph::PropHdriPath, "abandoned_garage_2k.exr");
	root.setProperty(scenegraph::PropHdriIntensity, "1.0");
	root.setProperty(scenegraph::PropGroundPlane, "false");
	root.setProperty(scenegraph::PropEnvHidden, "false");
	root.setProperty(scenegraph::PropRenderFilmic, "true");

	VENGIFormat format;
	const io::ArchivePtr &archive = helper_filesystemarchive();
	ASSERT_TRUE(format.saveGroups(sceneGraph, "material-test.vengi", archive, testSaveCtx))
		<< "Could not save material-test.vengi";

	scenegraph::SceneGraph loadedSceneGraph;
	ASSERT_TRUE(format.loadGroups("material-test.vengi", archive, loadedSceneGraph, testLoadCtx))
		<< "Could not reload material-test.vengi";
	for (int i = 0; i < clumpCount; ++i) {
		const scenegraph::SceneGraphNode *found = nullptr;
		for (auto iter = loadedSceneGraph.beginModel(); iter != loadedSceneGraph.end(); ++iter) {
			if ((*iter).name() == clumps[i].name) {
				found = &*iter;
				break;
			}
		}
		ASSERT_NE(nullptr, found) << "Reload lost material-test node " << clumps[i].name;
		const uint8_t materialIndex = (uint8_t)(i + 1);
		EXPECT_EQ(expectedPaletteSize, found->palette().size());
		EXPECT_EQ(clumps[i].name, found->palette().colorName(materialIndex));
		EXPECT_EQ(clumps[i].type, found->palette().material(materialIndex).type);
		EXPECT_EQ(materialIndex, found->volume()->voxel(10, 10, 10).getColor());
	}
	const scenegraph::SceneGraphNode *loadedGround = nullptr;
	for (auto iter = loadedSceneGraph.beginModel(); iter != loadedSceneGraph.end(); ++iter) {
		if ((*iter).name() == "ground") {
			loadedGround = &*iter;
			break;
		}
	}
	ASSERT_NE(nullptr, loadedGround);
	EXPECT_EQ(expectedPaletteSize, loadedGround->palette().size());
	EXPECT_EQ("ground", loadedGround->palette().colorName((uint8_t)(clumpCount + 1)));
	EXPECT_EQ((uint8_t)(clumpCount + 1), loadedGround->volume()->voxel(0, 0, 0).getColor());
}

} // namespace voxelformat
