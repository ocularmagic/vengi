/**
 * @file
 */

#include "voxelformat/private/mesh/MeshTri.h"
#include "app/tests/AbstractTest.h"
#include "color/ColorUtil.h"
#include "image/Image.h"
#include "voxelformat/private/mesh/MeshMaterial.h"

namespace voxelformat {

class MeshTriTest : public app::AbstractTest {};

TEST_F(MeshTriTest, testColorAt4x4) {
	constexpr int h = 4;
	constexpr int w = 4;
	constexpr color::RGBA buffer[w * h]{
		{255, 0, 0, 255},	{255, 255, 0, 255},	  {255, 0, 255, 255},	{255, 255, 255, 255},
		{0, 255, 0, 255},	{13, 255, 50, 255},	  {127, 127, 127, 255}, {255, 127, 0, 255},
		{255, 0, 0, 255},	{255, 60, 0, 255},	  {255, 0, 30, 255},	{127, 69, 255, 255},
		{127, 127, 0, 255}, {255, 127, 127, 255}, {255, 0, 127, 255},	{0, 127, 80, 255}};
	static_assert(sizeof(buffer) == (size_t)w * (size_t)h * sizeof(uint32_t), "Unexpected rgba buffer size");
	const image::ImagePtr &texture = image::createEmptyImage("4x4");
	texture->loadRGBA((const uint8_t *)buffer, w, h);
	ASSERT_TRUE(texture);
	ASSERT_EQ(w, texture->width());
	ASSERT_EQ(h, texture->height());

	for (int s = 0; s < 2; ++s) {
		const bool originUpperLeft = s == 0;
		SCOPED_TRACE(s);
		voxelformat::MeshTri meshTri;
		MeshMaterialArray meshMaterialArray;
		meshMaterialArray.emplace_back(createMaterial(texture));
		meshTri.materialIdx = meshMaterialArray.size() - 1;
		for (int x = 0; x < w; ++x) {
			for (int y = 0; y < h; ++y) {
				meshTri.setUVs(image::Image::uv(x, y, w, h, originUpperLeft),
							  image::Image::uv(x, y + 1, w, h, originUpperLeft),
							  image::Image::uv(x + 1, y, w, h, originUpperLeft));
				const glm::vec2 &uv = meshTri.centerUV();
				const color::RGBA color = colorAt(meshTri, meshMaterialArray, uv, originUpperLeft);
				const int texIndex = y * w + x;
				ASSERT_EQ(buffer[texIndex], color)
					<< "pixel(" << x << "/" << y << "), " << color::print(buffer[texIndex]) << " vs "
					<< color::print(color) << " ti: " << texIndex;
			}
		}
	}
}

TEST_F(MeshTriTest, testColorAtBilinearMidpoint) {
	const color::RGBA red(255, 0, 0, 255);
	const color::RGBA blue(0, 0, 255, 255);
	const color::RGBA pixels[2] = {red, blue};
	const image::ImagePtr &texture = image::createEmptyImage("2x1");
	texture->loadRGBA((const uint8_t *)pixels, 2, 1);
	MeshMaterialArray meshMaterialArray;
	meshMaterialArray.emplace_back(createMaterial(texture));
	meshMaterialArray[0]->uvOriginUpperLeft = true;
	voxelformat::MeshTri meshTri;
	meshTri.materialIdx = 0;
	meshTri.setUVs(glm::vec2(0.5f, 0.5f), glm::vec2(0.5f, 0.5f), glm::vec2(0.5f, 0.5f));
	const color::RGBA mid = colorAt(meshTri, meshMaterialArray, meshTri.centerUV(), true, true);
	EXPECT_NEAR(127.0f, (float)mid.r, 8.0f);
	EXPECT_NEAR(127.0f, (float)mid.b, 8.0f);
}

TEST_F(MeshTriTest, testColorAtIdentityColorFactorKeepsTexel) {
	constexpr color::RGBA texel(180, 210, 230, 255);
	const image::ImagePtr &texture = image::createEmptyImage("1x1");
	texture->loadRGBA((const uint8_t *)&texel, 1, 1);
	MeshMaterialArray meshMaterialArray;
	meshMaterialArray.emplace_back(createMaterial(texture));
	meshMaterialArray[0]->multiplyColorFactor = true;
	meshMaterialArray[0]->colorFactor = glm::vec4(1.0f);
	meshMaterialArray[0]->uvOriginUpperLeft = true;
	voxelformat::MeshTri meshTri;
	meshTri.materialIdx = 0;
	meshTri.setUVs(glm::vec2(0.5f, 0.5f), glm::vec2(0.5f, 0.5f), glm::vec2(0.5f, 0.5f));
	EXPECT_EQ(texel, colorAt(meshTri, meshMaterialArray, meshTri.centerUV()));
}

TEST_F(MeshTriTest, testClosestPointUv) {
	voxelformat::MeshTri meshTri;
	meshTri.setVertices(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f, 2.0f, 0.0f));
	meshTri.setUVs(glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f, 1.0f));

	glm::vec3 closest;
	glm::vec2 uv;
	meshTri.closestPoint(glm::vec3(0.0f, 0.0f, 0.0f), closest, uv);
	EXPECT_NEAR(0.0f, closest.x, 0.0001f);
	EXPECT_NEAR(0.0f, closest.y, 0.0001f);
	EXPECT_NEAR(0.0f, closest.z, 0.0001f);
	EXPECT_NEAR(0.0f, uv.x, 0.0001f);
	EXPECT_NEAR(0.0f, uv.y, 0.0001f);

	meshTri.closestPoint(glm::vec3(0.5f, 0.5f, 0.0f), closest, uv);
	EXPECT_NEAR(0.5f, closest.x, 0.0001f);
	EXPECT_NEAR(0.5f, closest.y, 0.0001f);
	EXPECT_NEAR(0.0f, closest.z, 0.0001f);
	EXPECT_NEAR(0.25f, uv.x, 0.0001f);
	EXPECT_NEAR(0.25f, uv.y, 0.0001f);

	// Off the triangle: clamp onto the AB edge, do not use the centroid UV.
	meshTri.closestPoint(glm::vec3(1.0f, -1.0f, 0.0f), closest, uv);
	EXPECT_NEAR(0.0f, closest.y, 0.0001f);
	EXPECT_NEAR(0.0f, uv.y, 0.0001f);
	EXPECT_GT(uv.x, 0.0f);
	EXPECT_LT(uv.x, 1.0f);
}

} // namespace voxelformat
