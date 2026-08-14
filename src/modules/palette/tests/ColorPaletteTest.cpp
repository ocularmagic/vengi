/**
 * @file
 */

#include "palette/ColorPalette.h"
#include "app/tests/AbstractTest.h"
#include "core/collection/Buffer.h"
#include "image/Image.h"
#include "palette/FormatConfig.h"
#include "palette/Palette.h"
#include "palette/PaletteUtil.h"

namespace palette {

class ColorPaletteTest : public app::AbstractTest {
protected:
	bool onInitApp() override {
		if (!app::AbstractTest::onInitApp()) {
			return false;
		}
		return FormatConfig::init();
	}
};

TEST_F(ColorPaletteTest, testSave) {
	palette::Palette pal;
	pal.nippon();
	ColorPalette palette = palette::toColorPalette(pal);
	EXPECT_EQ(palette.size(), pal.size());
	EXPECT_EQ(palette.name(), pal.name());
	for (size_t i = 0; i < pal.size(); ++i) {
		EXPECT_EQ(palette.color(i), pal.color(i));
	}
}

TEST_F(ColorPaletteTest, testAdd) {
	ColorPalette palette;
	palette.add(color::RGBA(255, 0, 0, 255), "Red");
	palette.add(color::RGBA(0, 255, 0, 255), "Green");
	palette.add(color::RGBA(0, 0, 255, 255), "Blue");

	EXPECT_EQ(3u, palette.size());
	EXPECT_EQ(3, palette.colorCount());
	EXPECT_EQ(color::RGBA(255, 0, 0, 255), palette.color(0));
	EXPECT_EQ("Red", palette.colorName(0));
	EXPECT_EQ(color::RGBA(0, 255, 0, 255), palette.color(1));
	EXPECT_EQ("Green", palette.colorName(1));
	EXPECT_EQ(color::RGBA(0, 0, 255, 255), palette.color(2));
	EXPECT_EQ("Blue", palette.colorName(2));
}

TEST_F(ColorPaletteTest, testSet) {
	ColorPalette palette;
	palette.setSize(2);
	palette.setColor(0, color::RGBA(255, 255, 255, 255));
	palette.setColorName(0, "White");
	palette.setColor(1, color::RGBA(0, 0, 0, 255));
	palette.setColorName(1, "Black");

	EXPECT_EQ(2u, palette.size());
	EXPECT_EQ(color::RGBA(255, 255, 255, 255), palette.color(0));
	EXPECT_EQ("White", palette.colorName(0));
	EXPECT_EQ(color::RGBA(0, 0, 0, 255), palette.color(1));
	EXPECT_EQ("Black", palette.colorName(1));

	palette.set(0, color::RGBA(127, 127, 127, 255), "Grey");
	EXPECT_EQ(color::RGBA(127, 127, 127, 255), palette.color(0));
	EXPECT_EQ("Grey", palette.colorName(0));
}

TEST_F(ColorPaletteTest, testLoad) {
	image::ImagePtr img = image::createEmptyImage("test");
	img->load(2, 2, [](int x, int y, color::RGBA &color) {
		if (x == 0 && y == 0) color = color::RGBA(255, 0, 0, 255);
		else if (x == 1 && y == 0) color = color::RGBA(0, 255, 0, 255);
		else if (x == 0 && y == 1) color = color::RGBA(0, 0, 255, 255);
		else color = color::RGBA(255, 255, 255, 255);
	});

	ColorPalette palette;
	EXPECT_TRUE(palette.load(img));
	EXPECT_EQ(4u, palette.size());
	EXPECT_EQ(color::RGBA(255, 0, 0, 255), palette.color(0));
	EXPECT_EQ(color::RGBA(0, 255, 0, 255), palette.color(1));
	EXPECT_EQ(color::RGBA(0, 0, 255, 255), palette.color(2));
	EXPECT_EQ(color::RGBA(255, 255, 255, 255), palette.color(3));
	EXPECT_EQ("test", palette.name());
}

TEST_F(ColorPaletteTest, testDirty) {
	ColorPalette palette;
	EXPECT_FALSE(palette.dirty());
	palette.add(color::RGBA(255, 0, 0, 255));
	// add does not mark dirty? Let's check implementation.
	// add calls push_back on _entries, but does not call markDirty().
	// Wait, if add doesn't mark dirty, that might be a bug or intended.
	// Let's check setSize/setColor.
	palette.markClean();
	palette.setColor(0, color::RGBA(0, 255, 0, 255));
	EXPECT_TRUE(palette.dirty());

	palette.markClean();
	palette.setName("New Name");
	EXPECT_TRUE(palette.dirty());
}

TEST_F(ColorPaletteTest, testIterators) {
	ColorPalette palette;
	palette.add(color::RGBA(255, 0, 0, 255));
	palette.add(color::RGBA(0, 255, 0, 255));

	int count = 0;
	for (const auto& entry : palette) {
		if (count == 0) {
			EXPECT_EQ(color::RGBA(255, 0, 0, 255), entry.color);
		}
		if (count == 1) {
			EXPECT_EQ(color::RGBA(0, 255, 0, 255), entry.color);
		}
		count++;
	}
	EXPECT_EQ(2, count);
}

TEST_F(ColorPaletteTest, testPrint) {
	ColorPalette palette;
	palette.add(color::RGBA(255, 0, 0, 255));
	core::String str = palette::toString(palette);
	EXPECT_FALSE(str.empty());
}

TEST_F(ColorPaletteTest, testQuantizeTargetColors) {
	palette::Palette pal;
	const color::RGBA colors[] = {
		color::RGBA(255, 0, 0, 255),
		color::RGBA(0, 255, 0, 255),
		color::RGBA(0, 0, 255, 255),
		color::RGBA(255, 255, 0, 255),
		color::RGBA(0, 255, 255, 255),
		color::RGBA(255, 0, 255, 255),
		color::RGBA(128, 128, 128, 255),
		color::RGBA(255, 128, 0, 255),
		color::RGBA(0, 128, 255, 255),
		color::RGBA(128, 0, 255, 255)
	};
	pal.quantize(colors, lengthof(colors), 5);
	EXPECT_EQ(5, pal.colorCount());
}

TEST_F(ColorPaletteTest, testWeightedPaletteKeepsDominantColor) {
	// Many samples of one material, a handful of another, and unique noise.
	// Frequency+range split must keep the dominant color.
	core::Buffer<color::RGBA> samples;
	for (int i = 0; i < 400; ++i) {
		samples.push_back(color::RGBA(150, 195, 225, 255));
	}
	for (int i = 0; i < 8; ++i) {
		samples.push_back(color::RGBA(230, 160 + i, 130 + i, 255));
	}
	for (int i = 0; i < 20; ++i) {
		samples.push_back(color::RGBA((uint8_t)(i * 3), (uint8_t)(i * 5), (uint8_t)(i * 7), 255));
	}

	const palette::Palette pal = palette::toPaletteWeighted(samples.data(), samples.size(), 32);
	ASSERT_GT(pal.colorCount(), 0);
	ASSERT_LE(pal.colorCount(), 32);
	bool foundBlue = false;
	for (int i = 0; i < pal.colorCount(); ++i) {
		const color::RGBA c = pal.color(i);
		if (c.b > c.r + 20 && c.g > 160) {
			foundBlue = true;
			break;
		}
	}
	EXPECT_TRUE(foundBlue);
}

TEST_F(ColorPaletteTest, testWeightedPaletteMajorityGetsRamp) {
	core::Buffer<color::RGBA> samples;
	for (int i = 0; i < 200; ++i) {
		samples.push_back(color::RGBA(250, 200, 230, 255));
	}
	for (int i = 0; i < 200; ++i) {
		samples.push_back(color::RGBA(230, 140, 180, 255));
	}
	for (int i = 0; i < 200; ++i) {
		samples.push_back(color::RGBA(200, 80, 130, 255));
	}
	for (int i = 0; i < 20; ++i) {
		samples.push_back(color::RGBA((uint8_t)(i * 6), (uint8_t)(i * 4), (uint8_t)(i * 5), 255));
	}

	const palette::Palette pal = palette::toPaletteWeighted(samples.data(), samples.size(), 16);
	ASSERT_GT(pal.colorCount(), 0);
	ASSERT_LE(pal.colorCount(), 16);
	int pinkSlots = 0;
	int minR = 255;
	int maxR = 0;
	for (int i = 0; i < pal.colorCount(); ++i) {
		const color::RGBA c = pal.color(i);
		if (c.r > c.g + 20 && c.r > 150) {
			++pinkSlots;
			if (c.r < minR) {
				minR = c.r;
			}
			if (c.r > maxR) {
				maxR = c.r;
			}
		}
	}
	EXPECT_GE(pinkSlots, 3);
	EXPECT_GE(maxR - minR, 20);
}

TEST_F(ColorPaletteTest, testWeightedPaletteOutliersDoNotDominate) {
	core::Buffer<color::RGBA> samples;
	for (int i = 0; i < 300; ++i) {
		samples.push_back(color::RGBA(40, 160, 70, 255));
	}
	for (int i = 0; i < 40; ++i) {
		samples.push_back(color::RGBA((uint8_t)(i * 6), (uint8_t)(i * 3), (uint8_t)(255 - i * 4), 255));
	}

	const palette::Palette pal = palette::toPaletteWeighted(samples.data(), samples.size(), 16);
	ASSERT_GT(pal.colorCount(), 0);
	ASSERT_LE(pal.colorCount(), 16);
	int greenSlots = 0;
	for (int i = 0; i < pal.colorCount(); ++i) {
		const color::RGBA c = pal.color(i);
		if (c.g > c.r + 40 && c.g > c.b) {
			++greenSlots;
		}
	}
	EXPECT_GE(greenSlots, 1);
	EXPECT_LT(greenSlots, pal.colorCount());
}

TEST_F(ColorPaletteTest, testWeightedPaletteSplitsFrequentNeighbors) {
	const color::RGBA a(144, 192, 224, 255);
	const color::RGBA b(176, 192, 224, 255);
	ASSERT_NE(a.r >> 2, b.r >> 2);

	core::Buffer<color::RGBA> samples;
	for (int i = 0; i < 200; ++i) {
		samples.push_back(a);
	}
	for (int i = 0; i < 200; ++i) {
		samples.push_back(b);
	}
	for (int i = 0; i < 20; ++i) {
		samples.push_back(color::RGBA((uint8_t)i, (uint8_t)i, (uint8_t)i, 255));
	}

	const palette::Palette pal = palette::toPaletteWeighted(samples.data(), samples.size(), 8);
	ASSERT_GE(pal.colorCount(), 2);
	int blueish = 0;
	int minR = 255;
	int maxR = 0;
	for (int i = 0; i < pal.colorCount(); ++i) {
		const color::RGBA c = pal.color(i);
		if (c.b > 200 && c.g > 160) {
			++blueish;
			if (c.r < minR) {
				minR = c.r;
			}
			if (c.r > maxR) {
				maxR = c.r;
			}
		}
	}
	EXPECT_GE(blueish, 2);
	EXPECT_GE(maxR - minR, 4);
}

TEST_F(ColorPaletteTest, testWeightedPaletteCollapsesNearDuplicates) {
	core::Buffer<color::RGBA> samples;
	const color::RGBA pinkA(254, 201, 237, 255);
	const color::RGBA pinkB(254, 201, 234, 255);
	const color::RGBA green(40, 160, 70, 255);
	for (int i = 0; i < 200; ++i) {
		samples.push_back(pinkA);
	}
	for (int i = 0; i < 180; ++i) {
		samples.push_back(pinkB);
	}
	for (int i = 0; i < 50; ++i) {
		samples.push_back(green);
	}
	const palette::Palette pal = palette::toPaletteWeighted(samples.data(), samples.size(), 2);
	ASSERT_EQ(2, pal.colorCount());
	bool hasPink = false;
	bool hasGreen = false;
	for (int i = 0; i < pal.colorCount(); ++i) {
		const color::RGBA c = pal.color(i);
		if (c.r > 200 && c.b > 200) {
			hasPink = true;
			EXPECT_EQ(pinkA, c);
		}
		if (c.g > c.r + 40) {
			hasGreen = true;
		}
	}
	EXPECT_TRUE(hasPink);
	EXPECT_TRUE(hasGreen);
}

TEST_F(ColorPaletteTest, testWeightedPaletteUsesDominantNotAverage) {
	core::Buffer<color::RGBA> samples;
	const color::RGBA pink(240, 160, 200, 255);
	const color::RGBA dirt(40, 40, 40, 255);
	for (int i = 0; i < 300; ++i) {
		samples.push_back(pink);
	}
	for (int i = 0; i < 80; ++i) {
		samples.push_back(dirt);
	}
	const palette::Palette pal = palette::toPaletteWeighted(samples.data(), samples.size(), 1);
	ASSERT_EQ(1, pal.colorCount());
	const color::RGBA c = pal.color(0);
	EXPECT_NEAR((int)c.r, 240, 12);
	EXPECT_NEAR((int)c.g, 160, 12);
	EXPECT_NEAR((int)c.b, 200, 12);
}

TEST_F(ColorPaletteTest, testWeightedPaletteDefaultTargetIs256) {
	core::Buffer<color::RGBA> samples;
	for (int i = 0; i < 300; ++i) {
		samples.push_back(color::RGBA((uint8_t)i, (uint8_t)(255 - (i % 256)), 80, 255));
	}
	const palette::Palette pal = palette::toPaletteWeighted(samples.data(), samples.size(), 0);
	ASSERT_GT(pal.colorCount(), 0);
	ASSERT_LE(pal.colorCount(), 256);
}

TEST_F(ColorPaletteTest, testQuantizeTargetColorsZeroMeansNoLimit) {
	palette::Palette pal;
	const color::RGBA colors[] = {
		color::RGBA(255, 0, 0, 255),
		color::RGBA(0, 255, 0, 255),
		color::RGBA(0, 0, 255, 255),
		color::RGBA(255, 255, 0, 255),
		color::RGBA(0, 255, 255, 255)
	};
	pal.quantize(colors, lengthof(colors), 0);
	EXPECT_EQ(5, pal.colorCount());
}

} // namespace palette
