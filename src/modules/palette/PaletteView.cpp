/**
 * @file
 */

#include "color/ColorUtil.h"
#include "PaletteView.h"
#include "app/Async.h"
#include "color/Color.h"
#include "core/Algorithm.h"
#include "core/Common.h"
#include "core/sdl/SDLSystem.h"
#include "palette/Palette.h"
#include <glm/gtc/constants.hpp>
#include <glm/vec3.hpp>
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/norm.hpp>

namespace palette {

// Lab chroma below this is treated as gray (hue is unstable).
static const float HueSortGrayChroma = 10.0f;
// Start a new family when neighboring hues jump more than this (0-1 circle).
static const float HueSortFamilyGap = 0.08f;
// Split a dense hue run if it spans more than this (keeps red vs yellow apart).
static const float HueSortMaxFamilySpan = 0.20f;

struct HueSortEntry {
	uint8_t index;
	int family;
	float lightness;
	float chroma;
	float hue;
	float labA;
	float labB;
	bool gray;
};

static float circularHueDelta(float from, float to) {
	float delta = to - from;
	if (delta < 0.0f) {
		delta += 1.0f;
	}
	return delta;
}

static bool hueSortLess(const HueSortEntry &lhs, const HueSortEntry &rhs) {
	if (lhs.family != rhs.family) {
		return lhs.family < rhs.family;
	}
	if (lhs.lightness > rhs.lightness) {
		return true;
	}
	if (lhs.lightness < rhs.lightness) {
		return false;
	}
	if (lhs.gray) {
		if (lhs.labA < rhs.labA) {
			return true;
		}
		if (lhs.labA > rhs.labA) {
			return false;
		}
		if (lhs.labB < rhs.labB) {
			return true;
		}
		if (lhs.labB > rhs.labB) {
			return false;
		}
	} else {
		if (lhs.hue < rhs.hue) {
			return true;
		}
		if (lhs.hue > rhs.hue) {
			return false;
		}
		if (lhs.chroma < rhs.chroma) {
			return true;
		}
		if (lhs.chroma > rhs.chroma) {
			return false;
		}
	}
	return lhs.index < rhs.index;
}

PaletteView::PaletteView(Palette *palette) : _palette(palette) {
	sortOriginal();
}

void PaletteView::exchangeUIIndices(uint8_t palettePanelIdx1, uint8_t palettePanelIdx2) {
	if (palettePanelIdx1 == palettePanelIdx2) {
		return;
	}
	core::exchange(_uiIndices[palettePanelIdx1], _uiIndices[palettePanelIdx2]);
	_palette->markDirty();
	_palette->markSave();
}

void PaletteView::sortOriginal() {
	for (int i = 0; i < PaletteMaxColors; ++i) {
		_uiIndices[i] = i;
	}
	_palette->markDirty();
}

void PaletteView::sortHue() {
	const int n = (int)_palette->size();
	HueSortEntry entries[PaletteMaxColors];
	int chromatic[PaletteMaxColors];
	int chromaticCount = 0;

	for (int i = 0; i < n; ++i) {
		const uint8_t idx = _uiIndices[i];
		float L = 0.0f;
		float a = 0.0f;
		float b = 0.0f;
		color::getCIELab(_palette->color(idx), L, a, b);
		const float chroma = SDL_sqrtf(a * a + b * b);
		const bool gray = chroma < HueSortGrayChroma;
		float hue = 0.0f;
		if (!gray) {
			hue = SDL_atan2f(b, a) / (2.0f * glm::pi<float>());
			if (hue < 0.0f) {
				hue += 1.0f;
			}
		}
		entries[i].index = idx;
		entries[i].family = 0;
		entries[i].lightness = L;
		entries[i].chroma = chroma;
		entries[i].hue = hue;
		entries[i].labA = a;
		entries[i].labB = b;
		entries[i].gray = gray;
		if (!gray) {
			chromatic[chromaticCount++] = i;
		}
	}

	int familyCount = 0;
	if (chromaticCount >= 2) {
		core::sort(chromatic, chromatic + chromaticCount, [&entries](int lhs, int rhs) {
			if (entries[lhs].hue < entries[rhs].hue) {
				return true;
			}
			if (entries[lhs].hue > entries[rhs].hue) {
				return false;
			}
			return entries[lhs].index < entries[rhs].index;
		});

		int cut = chromaticCount - 1;
		float bestGap = -1.0f;
		for (int i = 0; i < chromaticCount; ++i) {
			const int next = (i + 1) % chromaticCount;
			const float gap = circularHueDelta(entries[chromatic[i]].hue, entries[chromatic[next]].hue);
			if (gap > bestGap) {
				bestGap = gap;
				cut = i;
			}
		}

		const int start = (cut + 1) % chromaticCount;
		float familyStartHue = entries[chromatic[start]].hue;
		float prevHue = familyStartHue;
		entries[chromatic[start]].family = 0;
		for (int k = 1; k < chromaticCount; ++k) {
			const int pos = (start + k) % chromaticCount;
			const float hue = entries[chromatic[pos]].hue;
			const float gap = circularHueDelta(prevHue, hue);
			const float span = circularHueDelta(familyStartHue, hue);
			if (gap > HueSortFamilyGap || span > HueSortMaxFamilySpan) {
				++familyCount;
				familyStartHue = hue;
			}
			entries[chromatic[pos]].family = familyCount;
			prevHue = hue;
		}
		++familyCount;
	} else if (chromaticCount == 1) {
		entries[chromatic[0]].family = 0;
		familyCount = 1;
	}

	for (int i = 0; i < n; ++i) {
		if (entries[i].gray) {
			entries[i].family = familyCount;
		}
	}

	core::sort(entries, entries + n, hueSortLess);
	for (int i = 0; i < n; ++i) {
		_uiIndices[i] = entries[i].index;
	}
	_palette->markDirty();
}

void PaletteView::sortSaturation() {
	app::sort_parallel(_uiIndices, &_uiIndices[_palette->size()], [this](uint8_t lhs, uint8_t rhs) {
		float lhhue = 0.0f;
		float lhsaturation = 0.0f;
		float lhbrightness = 0.0f;

		float rhhue = 0.0f;
		float rhsaturation = 0.0f;
		float rhbrightness = 0.0f;

		color::getHSB(_palette->color(lhs), lhhue, lhsaturation, lhbrightness);
		color::getHSB(_palette->color(rhs), rhhue, rhsaturation, rhbrightness);
		return lhsaturation < rhsaturation;
	});
	_palette->markDirty();
}

void PaletteView::sortBrightness() {
	app::sort_parallel(_uiIndices, &_uiIndices[_palette->size()], [this](uint8_t lhs, uint8_t rhs) {
		return color::brightness(_palette->color(lhs)) < color::brightness(_palette->color(rhs));
	});
	_palette->markDirty();
}

void PaletteView::sortCIELab() {
	app::sort_parallel(_uiIndices, &_uiIndices[_palette->size()], [this](uint8_t lhs, uint8_t rhs) {
		glm::vec3 lcielab;
		glm::vec3 rcielab;
		color::getCIELab(_palette->color(lhs), lcielab.x, lcielab.y, lcielab.z);
		color::getCIELab(_palette->color(rhs), rcielab.x, rcielab.y, rcielab.z);
		return glm::length2(lcielab) < glm::length2(rcielab);
	});
	_palette->markDirty();
}

} // namespace palette
