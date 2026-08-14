/**
 * @file
 */

#include "voxelformat/private/image/PNGFormat.h"
#include "AbstractFormatTest.h"
#include "color/RGBA.h"
#include "core/ScopedPtr.h"
#include "image/Image.h"
#include "image/ImageType.h"
#include "io/Archive.h"
#include "io/MemoryArchive.h"
#include "io/Stream.h"
#include "palette/Palette.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "util/VarUtil.h"
#include "voxel/RawVolume.h"
#include "voxel/Region.h"
#include "voxel/Voxel.h"

namespace voxelformat {

class PNGFormatTest : public AbstractFormatTest {};

TEST_F(PNGFormatTest, testLoadPlane) {
	scenegraph::SceneGraph sceneGraph;
	testLoad(sceneGraph, "fuel_can.png", 1);
	scenegraph::SceneGraphNode *node = sceneGraph.firstModelNode();
	ASSERT_TRUE(node != nullptr);
	const voxel::Region &region = node->region();
	EXPECT_EQ(region.getDimensionsInVoxels(), glm::ivec3(128, 128, 1));
}

TEST_F(PNGFormatTest, testLoadVolume) {
	util::ScopedVarChange scoped(cfg::VoxformatImageImportType, PNGFormat::ImageType::Volume);
	scenegraph::SceneGraph sceneGraph;
	testLoad(sceneGraph, "test-heightmap.png", 1);
	scenegraph::SceneGraphNode *node = sceneGraph.firstModelNode();
	ASSERT_TRUE(node != nullptr);
	const voxel::Region &region = node->region();
	EXPECT_EQ(region.getDimensionsInVoxels(), glm::ivec3(8, 8, 3));
}

TEST_F(PNGFormatTest, testLoadHeightmap) {
	util::ScopedVarChange scoped(cfg::VoxformatImageImportType, PNGFormat::ImageType::Heightmap);
	scenegraph::SceneGraph sceneGraph;
	testLoad(sceneGraph, "test-heightmap.png", 1);
	scenegraph::SceneGraphNode *node = sceneGraph.firstModelNode();
	ASSERT_TRUE(node != nullptr);
	const voxel::Region &region = node->region();
	EXPECT_EQ(region.getDimensionsInVoxels(), glm::ivec3(8, 255, 8));
}

static bool loadSlice(const io::ArchivePtr &archive, const core::String &filename, image::Image &image) {
	core::ScopedPtr<io::SeekableReadStream> stream(archive->readStream(filename));
	if (!stream) {
		return false;
	}
	return image.load(image::ImageType::PNG, *stream, (int)stream->size());
}

TEST_F(PNGFormatTest, testSaveSlicesHollowsBuriedWhite) {
	util::ScopedVarChange saveType(cfg::VoxformatImageSaveType, PNGFormat::ImageType::Plane);
	util::ScopedVarChange hollow(cfg::VoxformatImageSliceHollowInterior, "true");

	const color::RGBA red(200, 40, 40, 255);
	const color::RGBA white(255, 255, 255, 255);
	palette::Palette pal;
	uint8_t redIdx = 0;
	uint8_t whiteIdx = 0;
	ASSERT_TRUE(pal.tryAdd(red, false, &redIdx, false));
	ASSERT_TRUE(pal.tryAdd(white, false, &whiteIdx, false));

	const voxel::Region region(0, 0, 0, 4, 4, 4);
	voxel::RawVolume volume(region);
	for (int z = 0; z <= 4; ++z) {
		for (int y = 0; y <= 4; ++y) {
			for (int x = 0; x <= 4; ++x) {
				const bool surface = x == 0 || x == 4 || y == 0 || y == 4 || z == 0 || z == 4;
				volume.setVoxel(x, y, z, voxel::createVoxel(voxel::VoxelType::Generic, surface ? redIdx : whiteIdx));
			}
		}
	}

	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	node.setUnownedVolume(&volume);
	node.setPalette(pal);
	const int nodeId = sceneGraph.emplace(core::move(node));
	ASSERT_NE(nodeId, InvalidNodeId);
	const core::String uuidStr = sceneGraph.node(nodeId).uuid().str();

	PNGFormat format;
	const io::ArchivePtr archive = io::openMemoryArchive();
	ASSERT_TRUE(format.save(sceneGraph, "hollow-slice.png", archive, testSaveCtx));

	const core::String midName = core::String::format("hollow-slice-%s-2.png", uuidStr.c_str());
	image::Image mid(midName);
	ASSERT_TRUE(loadSlice(archive, midName, mid));
	ASSERT_TRUE(mid.isLoaded());
	ASSERT_EQ(5, mid.width());
	ASSERT_EQ(5, mid.height());

	// Mid-height XZ cut. Image y is flipped from volume z (upper Z is row 0).
	EXPECT_EQ(red, mid.colorAt(0, 2));
	EXPECT_EQ(red, mid.colorAt(4, 2));
	EXPECT_EQ(white, mid.colorAt(1, 2));
	EXPECT_EQ(white, mid.colorAt(3, 2));
	EXPECT_EQ(white, mid.colorAt(2, 1));
	EXPECT_EQ(white, mid.colorAt(2, 3));
	EXPECT_EQ(color::RGBA(0, 0, 0, 0), mid.colorAt(2, 2));
}

TEST_F(PNGFormatTest, testSaveSlicesKeepsSurfaceWhite) {
	util::ScopedVarChange saveType(cfg::VoxformatImageSaveType, PNGFormat::ImageType::Plane);
	util::ScopedVarChange hollow(cfg::VoxformatImageSliceHollowInterior, "true");

	const color::RGBA white(255, 255, 255, 255);
	palette::Palette pal;
	uint8_t whiteIdx = 0;
	ASSERT_TRUE(pal.tryAdd(white, false, &whiteIdx, false));

	const voxel::Region region(0, 0, 0, 0, 0, 0);
	voxel::RawVolume volume(region);
	volume.setVoxel(0, 0, 0, voxel::createVoxel(voxel::VoxelType::Generic, whiteIdx));

	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	node.setUnownedVolume(&volume);
	node.setPalette(pal);
	const int nodeId = sceneGraph.emplace(core::move(node));
	ASSERT_NE(nodeId, InvalidNodeId);
	const core::String uuidStr = sceneGraph.node(nodeId).uuid().str();

	PNGFormat format;
	const io::ArchivePtr archive = io::openMemoryArchive();
	ASSERT_TRUE(format.save(sceneGraph, "surface-white.png", archive, testSaveCtx));

	const core::String name = core::String::format("surface-white-%s-0.png", uuidStr.c_str());
	image::Image img(name);
	ASSERT_TRUE(loadSlice(archive, name, img));
	ASSERT_TRUE(img.isLoaded());
	EXPECT_EQ(white, img.colorAt(0, 0));
}

TEST_F(PNGFormatTest, testSaveSlicesHollowInteriorDisabled) {
	util::ScopedVarChange saveType(cfg::VoxformatImageSaveType, PNGFormat::ImageType::Plane);
	util::ScopedVarChange hollow(cfg::VoxformatImageSliceHollowInterior, "false");

	const color::RGBA red(200, 40, 40, 255);
	const color::RGBA white(255, 255, 255, 255);
	palette::Palette pal;
	uint8_t redIdx = 0;
	uint8_t whiteIdx = 0;
	ASSERT_TRUE(pal.tryAdd(red, false, &redIdx, false));
	ASSERT_TRUE(pal.tryAdd(white, false, &whiteIdx, false));

	const voxel::Region region(0, 0, 0, 4, 4, 4);
	voxel::RawVolume volume(region);
	for (int z = 0; z <= 4; ++z) {
		for (int y = 0; y <= 4; ++y) {
			for (int x = 0; x <= 4; ++x) {
				const bool surface = x == 0 || x == 4 || y == 0 || y == 4 || z == 0 || z == 4;
				volume.setVoxel(x, y, z, voxel::createVoxel(voxel::VoxelType::Generic, surface ? redIdx : whiteIdx));
			}
		}
	}

	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	node.setUnownedVolume(&volume);
	node.setPalette(pal);
	const int nodeId = sceneGraph.emplace(core::move(node));
	ASSERT_NE(nodeId, InvalidNodeId);
	const core::String uuidStr = sceneGraph.node(nodeId).uuid().str();

	PNGFormat format;
	const io::ArchivePtr archive = io::openMemoryArchive();
	ASSERT_TRUE(format.save(sceneGraph, "solid-slice.png", archive, testSaveCtx));

	const core::String midName = core::String::format("solid-slice-%s-2.png", uuidStr.c_str());
	image::Image mid(midName);
	ASSERT_TRUE(loadSlice(archive, midName, mid));
	ASSERT_TRUE(mid.isLoaded());
	EXPECT_EQ(white, mid.colorAt(2, 2));
}

TEST_F(PNGFormatTest, testSaveSlicesTopDownXZ) {
	util::ScopedVarChange saveType(cfg::VoxformatImageSaveType, PNGFormat::ImageType::Plane);
	util::ScopedVarChange hollow(cfg::VoxformatImageSliceHollowInterior, "false");

	const color::RGBA red(200, 40, 40, 255);
	const color::RGBA blue(40, 40, 200, 255);
	palette::Palette pal;
	uint8_t redIdx = 0;
	uint8_t blueIdx = 0;
	ASSERT_TRUE(pal.tryAdd(red, false, &redIdx, false));
	ASSERT_TRUE(pal.tryAdd(blue, false, &blueIdx, false));

	const voxel::Region region(0, 0, 0, 2, 2, 2);
	voxel::RawVolume volume(region);
	// Top of the model (high Y) and front (+Z) at mid height.
	volume.setVoxel(1, 2, 1, voxel::createVoxel(voxel::VoxelType::Generic, redIdx));
	volume.setVoxel(1, 1, 2, voxel::createVoxel(voxel::VoxelType::Generic, blueIdx));

	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	node.setUnownedVolume(&volume);
	node.setPalette(pal);
	const int nodeId = sceneGraph.emplace(core::move(node));
	ASSERT_NE(nodeId, InvalidNodeId);
	const core::String uuidStr = sceneGraph.node(nodeId).uuid().str();

	PNGFormat format;
	const io::ArchivePtr archive = io::openMemoryArchive();
	ASSERT_TRUE(format.save(sceneGraph, "topdown.png", archive, testSaveCtx));

	const core::String topName = core::String::format("topdown-%s-2.png", uuidStr.c_str());
	const core::String midName = core::String::format("topdown-%s-1.png", uuidStr.c_str());
	const core::String zSliceName = core::String::format("topdown-%s-0.png", uuidStr.c_str());
	image::Image top(topName);
	image::Image mid(midName);
	ASSERT_TRUE(loadSlice(archive, topName, top));
	ASSERT_TRUE(loadSlice(archive, midName, mid));
	image::Image unused(zSliceName);
	EXPECT_FALSE(loadSlice(archive, zSliceName, unused));
	ASSERT_EQ(3, top.width());
	ASSERT_EQ(3, top.height());
	EXPECT_EQ(red, top.colorAt(1, 1));
	// Front (+Z) is the top row of the XZ image.
	EXPECT_EQ(blue, mid.colorAt(1, 0));
}

} // namespace voxelformat
