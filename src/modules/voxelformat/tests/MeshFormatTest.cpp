/**
 * @file
 */

#include "color/ColorUtil.h"
#include "voxelformat/private/mesh/Mesh.h"
#include "voxelformat/private/mesh/MeshFormat.h"
#include "color/Color.h"
#include "core/ConfigVar.h"
#include "core/tests/TestColorHelper.h"
#include "image/Image.h"
#include "io/Archive.h"
#include "palette/Palette.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "util/VarUtil.h"
#include "video/ShapeBuilder.h"
#include "voxel/MaterialColor.h"
#include "voxel/RawVolume.h"
#include "voxel/Voxel.h"
#include "voxelformat/VolumeFormat.h"
#include "voxelformat/private/mesh/MeshMaterial.h"
#include "voxelformat/tests/AbstractFormatTest.h"
#include "voxelutil/VolumeVisitor.h"
#include <limits>

namespace voxelformat {

class MeshFormatTest : public AbstractFormatTest {};

TEST_F(MeshFormatTest, testSubdivide) {
	MeshTriCollection tinyTris;
	voxelformat::MeshTri meshTri;
	meshTri.setVertices(glm::vec3(-8.77272797, -11.43335, -0.154544264),
						glm::vec3(-8.77272701, 11.1000004, -0.154543981),
						glm::vec3(8.77272701, 11.1000004, -0.154543981));
	MeshFormat::subdivideTri(meshTri, tinyTris, 0);
	EXPECT_EQ(1024u, tinyTris.size());
}

TEST_F(MeshFormatTest, testSubdivideRejectsTooLarge) {
	// Triangles whose AABB exceeds 2^(MaxSubdivideDepth - depth) cannot reach size <= 1
	// within the depth budget; early-out instead of exploding toward 4^depth leaves.
	MeshTriCollection tinyTris;
	voxelformat::MeshTri meshTri;
	meshTri.setVertices(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0e6f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0e6f, 0.0f));
	MeshFormat::subdivideTri(meshTri, tinyTris, 0);
	EXPECT_FALSE(tinyTris.empty());
	EXPECT_LT(tinyTris.size(), 16u);
}

TEST_F(MeshFormatTest, testSubdivideSkipsNonFinite) {
	MeshTriCollection tinyTris;
	voxelformat::MeshTri meshTri;
	const float inf = std::numeric_limits<float>::infinity();
	meshTri.setVertices(glm::vec3(inf, 0.0f, 0.0f), glm::vec3(0.0f, inf, 0.0f), glm::vec3(0.0f, 0.0f, inf));
	EXPECT_FALSE(MeshFormat::subdivideTri(meshTri, tinyTris, 0));
	EXPECT_TRUE(tinyTris.empty());
}

TEST_F(MeshFormatTest, testColorAt) {
	const image::ImagePtr &texture = image::loadImage("palette-nippon.png");
	ASSERT_TRUE(texture);
	ASSERT_EQ(256, texture->width());
	ASSERT_EQ(1, texture->height());

	palette::Palette pal;
	pal.nippon();

	MeshMaterialArray meshMaterialArray;
	voxelformat::MeshTri meshTri;
	meshMaterialArray.emplace_back(createMaterial(texture));
	meshTri.materialIdx = meshMaterialArray.size() - 1;
	for (int i = 0; i < 256; ++i) {
		const glm::vec2 uv = texture->uv(i, 0);
		meshTri.setUVs(uv, uv, uv);
		const color::RGBA color = colorAt(meshTri, meshMaterialArray, meshTri.centerUV());
		ASSERT_EQ(pal.color(i), color) << "i: " << i << " " << color::print(pal.color(i)) << " vs "
									   << color::print(color);
	}
}

TEST_F(MeshFormatTest, testCalculateAABB) {
	MeshTriCollection tris;
	voxelformat::MeshTri meshTri;

	{
		meshTri.setVertices(glm::vec3(0, 0, 0), glm::vec3(10, 0, 0), glm::vec3(10, 0, 10));
		tris.push_back(meshTri);
	}

	{
		meshTri.setVertices(glm::vec3(0, 0, 0), glm::vec3(-10, 0, 0), glm::vec3(-10, 0, -10));
		tris.push_back(meshTri);
	}

	glm::vec3 mins, maxs;
	ASSERT_TRUE(MeshFormat::calculateAABB(tris, mins, maxs));
	EXPECT_FLOAT_EQ(mins.x, -10.0f);
	EXPECT_FLOAT_EQ(mins.y, 0.0f);
	EXPECT_FLOAT_EQ(mins.z, -10.0f);
	EXPECT_FLOAT_EQ(maxs.x, 10.0f);
	EXPECT_FLOAT_EQ(maxs.y, 0.0f);
	EXPECT_FLOAT_EQ(maxs.z, 10.0f);
}

TEST_F(MeshFormatTest, testAreAllTrisAxisAligned) {
	MeshTriCollection tris;
	voxelformat::MeshTri meshTri;

	{
		meshTri.setVertices(glm::vec3(0, 0, 0), glm::vec3(10, 0, 0), glm::vec3(10, 0, 10));
		tris.push_back(meshTri);
	}

	{
		meshTri.setVertices(glm::vec3(0, 0, 0), glm::vec3(-10, 0, 0), glm::vec3(-10, 0, -10));
		tris.push_back(meshTri);
	}

	EXPECT_TRUE(MeshFormat::isVoxelMesh(tris));

	{
		meshTri.setVertices(glm::vec3(0, 0, 0), glm::vec3(-10, 1, 0), glm::vec3(-10, 0, -10));
		tris.push_back(meshTri);
	}

	EXPECT_FALSE(MeshFormat::isVoxelMesh(tris));
}

TEST_F(MeshFormatTest, testVoxelizeColor) {
	class TestMesh : public MeshFormat {
	public:
		bool saveMeshes(const core::Map<int, int> &, const scenegraph::SceneGraph &, const ChunkMeshes &,
						const core::String &, const io::ArchivePtr &, const glm::vec3 &, bool, bool, bool) override {
			return false;
		}
		void voxelize(scenegraph::SceneGraph &sceneGraph, Mesh &&mesh) {
			voxelizeMesh("test", sceneGraph, core::move(mesh));
			sceneGraph.updateTransforms();
		}
	};

	TestMesh testMesh;
	Mesh mesh;
	video::ShapeBuilder b;
	scenegraph::SceneGraph sceneGraph;

	palette::Palette pal;
	pal.nippon();
	const color::RGBA nipponRed = pal.color(37);
	const color::RGBA nipponBlue = pal.color(202);
	const float size = 10.0f;
	b.setPosition({size, 0.0f, size});
	b.setColor(color::fromRGBA(nipponRed));
	b.pyramid({size, size, size});

	const video::ShapeBuilder::Indices &indices = b.getIndices();
	const video::ShapeBuilder::Vertices &vertices = b.getVertices();

	// color of the tip is green
	video::ShapeBuilder::Colors colors = b.getColors();
	const color::RGBA nipponGreen = pal.color(145);
	colors[0] = color::fromRGBA(nipponGreen);
	colors[1] = color::fromRGBA(nipponBlue);

	const int n = (int)indices.size();
	for (int i = 0; i < n; i += 3) {
		voxelformat::MeshTri meshTri;
		meshTri.setVertices(vertices[indices[i]], vertices[indices[i + 1]], vertices[indices[i + 2]]);
		meshTri.setColor(color::getRGBA(colors[indices[i]]),
						 color::getRGBA(colors[indices[i + 1]]),
						 color::getRGBA(colors[indices[i + 2]]));
		mesh.addTriangle(meshTri);
	}
	testMesh.voxelize(sceneGraph, core::move(mesh));
	scenegraph::SceneGraphNode *node = sceneGraph.findNodeByName("test");
	ASSERT_NE(nullptr, node);
	const voxel::RawVolume *v = node->volume();
	const palette::Palette &nodePal = node->palette();
	EXPECT_EQ(v->voxel(0, 0, 0).getNormal(), NO_NORMAL);
	EXPECT_COLOR_NEAR(nipponRed, nodePal.color(v->voxel(0, 0, 0).getColor()), 0.06f);
	EXPECT_COLOR_NEAR(nipponRed, nodePal.color(v->voxel(size * 2 - 1, 0, size * 2 - 1).getColor()), 0.06f);
	EXPECT_COLOR_NEAR(nipponBlue, nodePal.color(v->voxel(0, 0, size * 2 - 1).getColor()), 0.06f);
	EXPECT_COLOR_NEAR(nipponRed, nodePal.color(v->voxel(size * 2 - 1, 0, 0).getColor()), 0.06f);
	EXPECT_COLOR_NEAR(nipponGreen, nodePal.color(v->voxel(size - 1, size - 1, size - 1).getColor()), 0.06f);
}

TEST_F(MeshFormatTest, testVoxelizeChunked) {
	class TestMesh : public MeshFormat {
	public:
		bool saveMeshes(const core::Map<int, int> &, const scenegraph::SceneGraph &, const ChunkMeshes &,
						const core::String &, const io::ArchivePtr &, const glm::vec3 &, bool, bool, bool) override {
			return false;
		}
		void voxelize(scenegraph::SceneGraph &sceneGraph, Mesh &&mesh) {
			voxelizeMesh("test", sceneGraph, core::move(mesh));
			sceneGraph.updateTransforms();
		}
	};

	// Enable chunked mode with small chunk size to force multiple chunks
	util::ScopedVarChange chunkedVar(cfg::VoxformatVoxelizeChunked, "true");
	util::ScopedVarChange chunkSizeVar(cfg::VoxformatVoxelizeChunkSize, "64");
	util::ScopedVarChange createPaletteVar(cfg::VoxelCreatePalette, "true");

	TestMesh testMesh;
	Mesh mesh;
	scenegraph::SceneGraph sceneGraph;

	palette::Palette pal;
	pal.nippon();
	const color::RGBA nipponRed = pal.color(37);

	// Create two large triangles forming a flat quad > 256 voxels wide to trigger chunked path.
	// The quad spans from (0,0,0) to (300,0,300) on the XZ plane.
	const float extent = 300.0f;
	{
		voxelformat::MeshTri meshTri;
		meshTri.setVertices(glm::vec3(0, 0, 0), glm::vec3(extent, 0, 0), glm::vec3(extent, 0, extent));
		meshTri.setColor(nipponRed, nipponRed, nipponRed);
		mesh.addTriangle(meshTri);
	}
	{
		voxelformat::MeshTri meshTri;
		meshTri.setVertices(glm::vec3(0, 0, 0), glm::vec3(extent, 0, extent), glm::vec3(0, 0, extent));
		meshTri.setColor(nipponRed, nipponRed, nipponRed);
		mesh.addTriangle(meshTri);
	}

	testMesh.voxelize(sceneGraph, core::move(mesh));

	// Should have created a group node containing multiple chunk children
	const scenegraph::SceneGraphNode *groupNode = sceneGraph.findNodeByName("test");
	ASSERT_NE(nullptr, groupNode);
	ASSERT_EQ(scenegraph::SceneGraphNodeType::Group, groupNode->type());

	// Count model children - with 300 voxels and 64-chunk size we expect multiple chunks
	int modelCount = 0;
	int totalVoxels = 0;
	for (auto iter = sceneGraph.beginModel(); iter != sceneGraph.end(); ++iter) {
		const scenegraph::SceneGraphNode &node = *iter;
		++modelCount;
		const voxel::RawVolume *volume = node.volume();
		ASSERT_NE(nullptr, volume);
		// Verify voxels have the expected color
		const palette::Palette &nodePal = node.palette();
		voxelutil::visitVolume(
			*volume,
			[&totalVoxels, &nodePal, &nipponRed](int, int, int, const voxel::Voxel &voxel) {
				++totalVoxels;
				EXPECT_COLOR_NEAR(nipponRed, nodePal.color(voxel.getColor()), 0.1f);
			},
			voxelutil::VisitAll());
	}
	EXPECT_GT(modelCount, 1) << "Chunked voxelization should produce multiple chunk nodes";
	EXPECT_GT(totalVoxels, 0) << "Chunks should contain voxels";
}

static void addColoredQuad(Mesh &mesh, const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c, const glm::vec3 &d,
						   color::RGBA color) {
	voxelformat::MeshTri t0;
	t0.setVertices(a, b, c);
	t0.setColor(color, color, color);
	mesh.addTriangle(t0);
	voxelformat::MeshTri t1;
	t1.setVertices(a, c, d);
	t1.setColor(color, color, color);
	mesh.addTriangle(t1);
}

TEST_F(MeshFormatTest, testSolidFillsInteriorWithSurfaceColor) {
	class TestMesh : public MeshFormat {
	public:
		bool saveMeshes(const core::Map<int, int> &, const scenegraph::SceneGraph &, const ChunkMeshes &,
						const core::String &, const io::ArchivePtr &, const glm::vec3 &, bool, bool, bool) override {
			return false;
		}
		void voxelize(scenegraph::SceneGraph &sceneGraph, Mesh &&mesh) {
			voxelizeMesh("solidbox", sceneGraph, core::move(mesh));
			sceneGraph.updateTransforms();
		}
	};

	util::ScopedVarChange modeVar(cfg::VoxformatVoxelizeMode, MeshFormat::VoxelizeMode::Solid);
	util::ScopedVarChange fillVar(cfg::VoxformatFillHollow, "true");
	util::ScopedVarChange paletteVar(cfg::VoxelCreatePalette, "true");

	const color::RGBA red(200, 40, 40, 255);
	const float s = 8.0f;
	Mesh mesh;
	addColoredQuad(mesh, {0, 0, 0}, {s, 0, 0}, {s, 0, s}, {0, 0, s}, red);
	addColoredQuad(mesh, {0, s, 0}, {0, s, s}, {s, s, s}, {s, s, 0}, red);
	addColoredQuad(mesh, {0, 0, 0}, {0, s, 0}, {s, s, 0}, {s, 0, 0}, red);
	addColoredQuad(mesh, {0, 0, s}, {s, 0, s}, {s, s, s}, {0, s, s}, red);
	addColoredQuad(mesh, {0, 0, 0}, {0, 0, s}, {0, s, s}, {0, s, 0}, red);
	addColoredQuad(mesh, {s, 0, 0}, {s, s, 0}, {s, s, s}, {s, 0, s}, red);

	TestMesh testMesh;
	scenegraph::SceneGraph sceneGraph;
	testMesh.voxelize(sceneGraph, core::move(mesh));
	scenegraph::SceneGraphNode *node = sceneGraph.findNodeByName("solidbox");
	ASSERT_NE(nullptr, node);
	const voxel::RawVolume *v = node->volume();
	ASSERT_NE(nullptr, v);

	int solidCount = 0;
	int surfaceRed = 0;
	const palette::Palette &pal = node->palette();
	voxelutil::visitVolume(
		*v,
		[&](int, int, int, const voxel::Voxel &voxel) {
			++solidCount;
			const color::RGBA c = pal.color(voxel.getColor());
			const int dr = (int)c.r > (int)red.r ? (int)c.r - (int)red.r : (int)red.r - (int)c.r;
			const int dg = (int)c.g > (int)red.g ? (int)c.g - (int)red.g : (int)red.g - (int)c.g;
			const int db = (int)c.b > (int)red.b ? (int)c.b - (int)red.b : (int)red.b - (int)c.b;
			if (dr + dg + db < 48) {
				++surfaceRed;
			}
		},
		voxelutil::VisitSolid());

	// Surface-only would be roughly 6*8*8 = 384; a filled 8^3 is 512.
	EXPECT_GT(solidCount, 400);
	EXPECT_GT(surfaceRed, 100);

	const glm::ivec3 center = v->region().getCenter();
	const voxel::Voxel centerVoxel = v->voxel(center);
	ASSERT_FALSE(voxel::isAir(centerVoxel.getMaterial()));
	EXPECT_EQ(centerVoxel.getNormal(), NO_NORMAL);
	int withNormal = 0;
	voxelutil::visitVolume(
		*v,
		[&](int, int, int, const voxel::Voxel &voxel) {
			if (voxel.getNormal() != NO_NORMAL) {
				++withNormal;
			}
		},
		voxelutil::VisitSolid());
	EXPECT_EQ(0, withNormal) << "Mesh voxelize must not stamp triangle normals onto cubes";
	EXPECT_COLOR_NEAR(color::RGBA(255, 255, 255, 255), pal.color(centerVoxel.getColor()), 0.05f);
	EXPECT_COLOR_NEAR(red, pal.color(v->voxel(v->region().getLowerX(), v->region().getCenter().y,
											 v->region().getCenter().z)
										.getColor()),
					  0.12f);
}

TEST_F(MeshFormatTest, testSolidNearestSurfaceColor) {
	class TestMesh : public MeshFormat {
	public:
		bool saveMeshes(const core::Map<int, int> &, const scenegraph::SceneGraph &, const ChunkMeshes &,
						const core::String &, const io::ArchivePtr &, const glm::vec3 &, bool, bool, bool) override {
			return false;
		}
		void voxelize(scenegraph::SceneGraph &sceneGraph, Mesh &&mesh) {
			voxelizeMesh("nearest", sceneGraph, core::move(mesh));
			sceneGraph.updateTransforms();
		}
	};

	util::ScopedVarChange modeVar(cfg::VoxformatVoxelizeMode, MeshFormat::VoxelizeMode::Solid);
	util::ScopedVarChange fillVar(cfg::VoxformatFillHollow, "false");
	util::ScopedVarChange paletteVar(cfg::VoxelCreatePalette, "true");

	const color::RGBA red(220, 20, 20, 255);
	const color::RGBA blue(20, 20, 220, 255);
	Mesh mesh;
	// Two disjoint quads far apart on Z so nearest-surface color is unambiguous.
	addColoredQuad(mesh, {0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0}, red);
	addColoredQuad(mesh, {0, 0, 12}, {4, 0, 12}, {4, 4, 12}, {0, 4, 12}, blue);

	TestMesh testMesh;
	scenegraph::SceneGraph sceneGraph;
	testMesh.voxelize(sceneGraph, core::move(mesh));
	scenegraph::SceneGraphNode *node = sceneGraph.findNodeByName("nearest");
	ASSERT_NE(nullptr, node);
	const voxel::RawVolume *v = node->volume();
	ASSERT_NE(nullptr, v);
	const palette::Palette &pal = node->palette();

	const voxel::Voxel nearRed = v->voxel(v->region().getLowerX() + 2, v->region().getLowerY() + 2,
										  v->region().getLowerZ());
	const voxel::Voxel nearBlue = v->voxel(v->region().getLowerX() + 2, v->region().getLowerY() + 2,
										   v->region().getUpperZ());
	ASSERT_FALSE(voxel::isAir(nearRed.getMaterial()));
	ASSERT_FALSE(voxel::isAir(nearBlue.getMaterial()));
	EXPECT_COLOR_NEAR(red, pal.color(nearRed.getColor()), 0.1f);
	EXPECT_COLOR_NEAR(blue, pal.color(nearBlue.getColor()), 0.1f);
}

TEST_F(MeshFormatTest, testSolidInteriorUsesNearestSurface) {
	class TestMesh : public MeshFormat {
	public:
		bool saveMeshes(const core::Map<int, int> &, const scenegraph::SceneGraph &, const ChunkMeshes &,
						const core::String &, const io::ArchivePtr &, const glm::vec3 &, bool, bool, bool) override {
			return false;
		}
		void voxelize(scenegraph::SceneGraph &sceneGraph, Mesh &&mesh) {
			voxelizeMesh("nearestinterior", sceneGraph, core::move(mesh));
			sceneGraph.updateTransforms();
		}
	};

	util::ScopedVarChange modeVar(cfg::VoxformatVoxelizeMode, MeshFormat::VoxelizeMode::Solid);
	util::ScopedVarChange fillVar(cfg::VoxformatFillHollow, "true");
	util::ScopedVarChange paletteVar(cfg::VoxelCreatePalette, "true");

	const color::RGBA red(220, 20, 20, 255);
	const color::RGBA blue(20, 20, 220, 255);
	const color::RGBA gray(140, 140, 140, 255);
	const float s = 8.0f;
	Mesh mesh;
	addColoredQuad(mesh, {0, 0, 0}, {s, 0, 0}, {s, 0, s}, {0, 0, s}, blue);
	addColoredQuad(mesh, {0, s, 0}, {0, s, s}, {s, s, s}, {s, s, 0}, red);
	addColoredQuad(mesh, {0, 0, 0}, {0, s, 0}, {s, s, 0}, {s, 0, 0}, gray);
	addColoredQuad(mesh, {0, 0, s}, {s, 0, s}, {s, s, s}, {0, s, s}, gray);
	addColoredQuad(mesh, {0, 0, 0}, {0, 0, s}, {0, s, s}, {0, s, 0}, gray);
	addColoredQuad(mesh, {s, 0, 0}, {s, s, 0}, {s, s, s}, {s, 0, s}, gray);

	TestMesh testMesh;
	scenegraph::SceneGraph sceneGraph;
	testMesh.voxelize(sceneGraph, core::move(mesh));
	scenegraph::SceneGraphNode *node = sceneGraph.findNodeByName("nearestinterior");
	ASSERT_NE(nullptr, node);
	const voxel::RawVolume *v = node->volume();
	ASSERT_NE(nullptr, v);
	const palette::Palette &pal = node->palette();

	const int x = v->region().getLowerX() + 4;
	const int z = v->region().getLowerZ() + 4;
	const voxel::Voxel top = v->voxel(x, v->region().getUpperY(), z);
	const voxel::Voxel bottom = v->voxel(x, v->region().getLowerY(), z);
	const voxel::Voxel center = v->voxel(x, v->region().getCenter().y, z);
	ASSERT_FALSE(voxel::isAir(top.getMaterial()));
	ASSERT_FALSE(voxel::isAir(bottom.getMaterial()));
	ASSERT_FALSE(voxel::isAir(center.getMaterial()));
	EXPECT_COLOR_NEAR(red, pal.color(top.getColor()), 0.12f);
	EXPECT_COLOR_NEAR(blue, pal.color(bottom.getColor()), 0.12f);
	EXPECT_COLOR_NEAR(color::RGBA(255, 255, 255, 255), pal.color(center.getColor()), 0.05f);
}

TEST_F(MeshFormatTest, testSolidPrefersDominantTriangle) {
	class TestMesh : public MeshFormat {
	public:
		bool saveMeshes(const core::Map<int, int> &, const scenegraph::SceneGraph &, const ChunkMeshes &,
						const core::String &, const io::ArchivePtr &, const glm::vec3 &, bool, bool, bool) override {
			return false;
		}
		void voxelize(scenegraph::SceneGraph &sceneGraph, Mesh &&mesh) {
			voxelizeMesh("dominant", sceneGraph, core::move(mesh));
			sceneGraph.updateTransforms();
		}
	};

	util::ScopedVarChange modeVar(cfg::VoxformatVoxelizeMode, MeshFormat::VoxelizeMode::Solid);
	util::ScopedVarChange fillVar(cfg::VoxformatFillHollow, "false");
	util::ScopedVarChange paletteVar(cfg::VoxelCreatePalette, "true");

	const color::RGBA red(220, 20, 20, 255);
	const color::RGBA blue(20, 20, 220, 255);
	Mesh mesh;
	addColoredQuad(mesh, {0, 0, 0}, {8, 0, 0}, {8, 8, 0}, {0, 8, 0}, red);
	// Tiny sliver in the middle of the same plane - must not win the voxel color.
	voxelformat::MeshTri sliver;
	sliver.setVertices(glm::vec3(3.9f, 3.9f, 0.0f), glm::vec3(4.1f, 3.9f, 0.0f), glm::vec3(4.0f, 4.1f, 0.0f));
	sliver.setColor(blue, blue, blue);
	mesh.addTriangle(sliver);

	TestMesh testMesh;
	scenegraph::SceneGraph sceneGraph;
	testMesh.voxelize(sceneGraph, core::move(mesh));
	scenegraph::SceneGraphNode *node = sceneGraph.findNodeByName("dominant");
	ASSERT_NE(nullptr, node);
	const voxel::RawVolume *v = node->volume();
	ASSERT_NE(nullptr, v);
	const palette::Palette &pal = node->palette();

	const voxel::Voxel mid = v->voxel(v->region().getLowerX() + 4, v->region().getLowerY() + 4,
									  v->region().getLowerZ());
	ASSERT_FALSE(voxel::isAir(mid.getMaterial()));
	EXPECT_COLOR_NEAR(red, pal.color(mid.getColor()), 0.1f);
}

TEST_F(MeshFormatTest, testSaveAsPointCloudUsesVoxelCenters) {
	class TestMesh : public MeshFormat {
	public:
		mutable PointCloud savedPointCloud;

		bool saveMeshes(const core::Map<int, int> &, const scenegraph::SceneGraph &, const ChunkMeshes &,
						const core::String &, const io::ArchivePtr &, const glm::vec3 &, bool, bool, bool) override {
			return false;
		}

		bool savePointCloud(const scenegraph::SceneGraph &, const PointCloud &pointCloud, const core::String &,
							const io::ArchivePtr &, const glm::vec3 &, bool) const override {
			savedPointCloud = pointCloud;
			return true;
		}
	};

	TestMesh testMesh;
	scenegraph::SceneGraph sceneGraph;
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	voxel::RawVolume *volume = new voxel::RawVolume(voxel::Region(glm::ivec3(0), glm::ivec3(1, 0, 0)));
	palette::Palette palette;
	palette.nippon();
	const color::RGBA nipponRed = palette.color(37);
	volume->setVoxel(0, 0, 0, voxel::createVoxel(palette, 37));
	volume->setVoxel(1, 0, 0, voxel::createVoxel(palette, 37));
	node.setVolume(volume);
	node.setPalette(palette);
	sceneGraph.emplace(core::move(node));

	util::ScopedVarChange pointCloudVarChange(cfg::VoxformatPointCloud, "true");
	ASSERT_TRUE(testMesh.saveGroups(sceneGraph, "test.ply", nullptr, {}));

	ASSERT_EQ(2u, testMesh.savedPointCloud.size());
	EXPECT_EQ(glm::vec3(0.5f, 0.5f, 0.5f), testMesh.savedPointCloud[0].position);
	EXPECT_EQ(glm::vec3(1.5f, 0.5f, 0.5f), testMesh.savedPointCloud[1].position);
	EXPECT_EQ(nipponRed, testMesh.savedPointCloud[0].color);
	EXPECT_EQ(nipponRed, testMesh.savedPointCloud[1].color);
}

} // namespace voxelformat
