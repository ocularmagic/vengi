/**
 * @file
 */

#include "PaletteUtil.h"
#include "color/ColorUtil.h"
#include "color/RGBA.h"
#include "core/Common.h"
#include "core/Log.h"
#include "core/collection/Buffer.h"
#include "core/collection/DynamicArray.h"
#include "core/collection/DynamicMap.h"
#include "palette/Palette.h"

namespace palette {

palette::Palette toPalette(const palette::ColorPalette &colorPalette) {
	palette::Palette palette;
	palette.setName(colorPalette.name());
	palette.setFilename(colorPalette.filename());
	if (colorPalette.size() <= PaletteMaxColors) {
		palette.setSize(colorPalette.size());
		for (size_t i = 0; i < colorPalette.size(); ++i) {
			palette.setColor(i, colorPalette.color(i));
			palette.setColorName(i, colorPalette.colorName(i));
			palette.setMaterial(i, colorPalette.material(i));
		}
	} else {
		const size_t colorCount = (int)colorPalette.size();
		core::Buffer<color::RGBA> colorBuffer;
		colorBuffer.reserve(colorCount);
		for (const auto &e : colorPalette) {
			colorBuffer.push_back(e.color);
		}
		palette.quantize(colorBuffer.data(), colorBuffer.size());
		if ((int)colorBuffer.size() != (int)palette.colorCount()) {
			Log::info("Loaded %i colors and quanitized to %i", (int)colorCount, palette.colorCount());
		}

		for (const auto &entry : colorPalette) {
			const int palIdx = palette.getClosestMatch(entry.color);
			if (palIdx == PaletteColorNotFound) {
				continue;
			}
			palette.setColorName(palIdx, entry.name);
			palette.setMaterial(palIdx, entry.material);
		}
	}
	palette.markDirty();
	return palette;
}

palette::Palette toPaletteQuantized(const palette::ColorPalette &colorPalette, int targetColors) {
	if (colorPalette.size() <= PaletteMaxColors && (targetColors <= 0 || (int)colorPalette.size() <= targetColors)) {
		return toPalette(colorPalette);
	}
	palette::Palette palette;
	palette.setName(colorPalette.name());
	palette.setFilename(colorPalette.filename());
	const size_t colorCount = colorPalette.size();
	core::Buffer<color::RGBA> colorBuffer;
	colorBuffer.reserve(colorCount);
	for (const auto &e : colorPalette) {
		colorBuffer.push_back(e.color);
	}
	palette.quantize(colorBuffer.data(), colorBuffer.size(), targetColors > 0 ? targetColors : 0);
	if ((int)colorBuffer.size() != (int)palette.colorCount()) {
		Log::info("Loaded %i colors and quantized to %i", (int)colorCount, palette.colorCount());
	}
	for (const auto &entry : colorPalette) {
		const int palIdx = palette.getClosestMatch(entry.color);
		if (palIdx == PaletteColorNotFound) {
			continue;
		}
		palette.setColorName(palIdx, entry.name);
		palette.setMaterial(palIdx, entry.material);
	}
	palette.markDirty();
	return palette;
}

namespace {

struct WeightedBin {
	color::RGBA c{0, 0, 0, 255};
	uint32_t n = 0;
	uint8_t avgR() const {
		return c.r;
	}
	uint8_t avgG() const {
		return c.g;
	}
	uint8_t avgB() const {
		return c.b;
	}
};

static void quantizeBinsToRamps(const core::DynamicArray<WeightedBin> &bins, int maxColors,
								core::DynamicArray<color::RGBA> &out) {
	if (bins.empty() || maxColors < 1) {
		return;
	}
	if ((int)bins.size() <= maxColors) {
		core::DynamicArray<WeightedBin> sorted = bins;
		sorted.sort([](const WeightedBin &a, const WeightedBin &b) { return a.n < b.n; });
		for (int i = 0; i < (int)sorted.size(); ++i) {
			out.push_back(sorted[i].c);
		}
		return;
	}

	struct Box {
		core::DynamicArray<int> idx;
		uint32_t n = 0;
		uint8_t minR = 255, maxR = 0;
		uint8_t minG = 255, maxG = 0;
		uint8_t minB = 255, maxB = 0;

		void add(int binIndex, const WeightedBin &bin) {
			idx.push_back(binIndex);
			n += bin.n;
			const uint8_t ar = bin.avgR();
			const uint8_t ag = bin.avgG();
			const uint8_t ab = bin.avgB();
			if (ar < minR) {
				minR = ar;
			}
			if (ar > maxR) {
				maxR = ar;
			}
			if (ag < minG) {
				minG = ag;
			}
			if (ag > maxG) {
				maxG = ag;
			}
			if (ab < minB) {
				minB = ab;
			}
			if (ab > maxB) {
				maxB = ab;
			}
		}

		int rangeR() const {
			return (int)maxR - (int)minR;
		}
		int rangeG() const {
			return (int)maxG - (int)minG;
		}
		int rangeB() const {
			return (int)maxB - (int)minB;
		}
		bool canSplit() const {
			return idx.size() > 1 && (rangeR() > 0 || rangeG() > 0 || rangeB() > 0);
		}
		uint64_t importance() const {
			return (uint64_t)n * (uint64_t)(rangeR() + rangeG() + rangeB());
		}
	};

	core::DynamicArray<Box> boxes;
	boxes.reserve(maxColors);
	{
		Box root;
		root.idx.reserve((int)bins.size());
		for (int i = 0; i < (int)bins.size(); ++i) {
			root.add(i, bins[i]);
		}
		boxes.push_back(core::move(root));
	}

	while ((int)boxes.size() < maxColors) {
		int best = -1;
		uint64_t bestImp = 0;
		for (int i = 0; i < (int)boxes.size(); ++i) {
			if (!boxes[i].canSplit()) {
				continue;
			}
			const uint64_t imp = boxes[i].importance();
			if (best < 0 || imp > bestImp) {
				bestImp = imp;
				best = i;
			}
		}
		if (best < 0) {
			break;
		}

		Box src = core::move(boxes[best]);
		boxes.erase(best);

		int channel = 0;
		int longest = src.rangeR();
		if (src.rangeG() > longest) {
			channel = 1;
			longest = src.rangeG();
		}
		if (src.rangeB() > longest) {
			channel = 2;
		}

		src.idx.sort([&](int ia, int ib) {
			uint8_t va;
			uint8_t vb;
			if (channel == 0) {
				va = bins[ia].avgR();
				vb = bins[ib].avgR();
			} else if (channel == 1) {
				va = bins[ia].avgG();
				vb = bins[ib].avgG();
			} else {
				va = bins[ia].avgB();
				vb = bins[ib].avgB();
			}
			return va > vb;
		});

		auto channelVal = [&](int binIndex) -> uint8_t {
			if (channel == 0) {
				return bins[binIndex].avgR();
			}
			if (channel == 1) {
				return bins[binIndex].avgG();
			}
			return bins[binIndex].avgB();
		};

		int split = 1;
		uint64_t bestScore = 0;
		uint32_t acc = 0;
		for (int i = 0; i < (int)src.idx.size() - 1; ++i) {
			acc += bins[src.idx[i]].n;
			const int gap = (int)channelVal(src.idx[i + 1]) - (int)channelVal(src.idx[i]);
			if (gap <= 0) {
				continue;
			}
			const uint32_t leftN = acc;
			const uint32_t rightN = src.n - acc;
			if (leftN == 0 || rightN == 0) {
				continue;
			}
			const uint32_t smaller = leftN < rightN ? leftN : rightN;
			const uint64_t score = (uint64_t)gap * (uint64_t)smaller;
			if (score > bestScore) {
				bestScore = score;
				split = i + 1;
			}
		}
		if (split <= 0) {
			split = 1;
		}
		if (split >= (int)src.idx.size()) {
			split = (int)src.idx.size() - 1;
		}

		Box left;
		Box right;
		left.idx.reserve(split);
		right.idx.reserve((int)src.idx.size() - split);
		for (int i = 0; i < split; ++i) {
			left.add(src.idx[i], bins[src.idx[i]]);
		}
		for (int i = split; i < (int)src.idx.size(); ++i) {
			right.add(src.idx[i], bins[src.idx[i]]);
		}
		if (left.n == 0 || right.n == 0) {
			boxes.push_back(core::move(src));
			break;
		}
		boxes.push_back(core::move(left));
		boxes.push_back(core::move(right));
	}

	for (int i = 0; i < (int)boxes.size(); ++i) {
		const Box &box = boxes[i];
		int best = -1;
		uint32_t bestN = 0;
		for (int j = 0; j < (int)box.idx.size(); ++j) {
			const WeightedBin &bin = bins[box.idx[j]];
			if (best < 0 || bin.n > bestN) {
				best = box.idx[j];
				bestN = bin.n;
			}
		}
		if (best >= 0) {
			out.push_back(bins[best].c);
		}
	}
}

} // namespace

palette::Palette toPaletteWeighted(const color::RGBA *samples, size_t sampleCount, int targetColors) {
	palette::Palette palette;
	if (samples == nullptr || sampleCount == 0) {
		return palette;
	}

	int maxColors = targetColors > 0 ? targetColors : 256;
	if (maxColors > PaletteMaxColors) {
		maxColors = PaletteMaxColors;
	}
	if (maxColors < 1) {
		maxColors = 1;
	}

	struct Bin {
		color::RGBA c{0, 0, 0, 255};
		uint32_t n = 0;
		uint8_t avgR() const {
			return c.r;
		}
		uint8_t avgG() const {
			return c.g;
		}
		uint8_t avgB() const {
			return c.b;
		}
		color::RGBA color() const {
			return c;
		}
	};

	// Exact surface colors, weighted by voxel count. Do not pre-average:
	// averages of petal+shadow become muddy palette slots.
	core::DynamicMap<color::RGBA, uint32_t, 1031, color::RGBAHasher> counts;
	uint32_t total = 0;
	for (size_t i = 0; i < sampleCount; ++i) {
		const color::RGBA c = samples[i];
		if (c.a == 0) {
			continue;
		}
		auto iter = counts.find(c);
		if (iter == counts.end()) {
			counts.put(c, 1u);
		} else {
			++iter->value;
		}
		++total;
	}
	if (counts.empty() || total == 0) {
		return palette;
	}

	core::DynamicArray<Bin> bins;
	bins.reserve((int)counts.size());
	for (const auto &e : counts) {
		Bin b;
		b.c = e->key;
		b.n = e->value;
		bins.push_back(b);
	}

	if ((int)bins.size() <= maxColors) {
		bins.sort([](const Bin &a, const Bin &b) { return a.n < b.n; });
		palette.setSize((int)bins.size());
		for (int i = 0; i < (int)bins.size(); ++i) {
			palette.setColor(i, bins[i].color());
		}
		Log::info("Loaded %i unique colors from %i voxels", (int)bins.size(), (int)total);
		palette.markDirty();
		return palette;
	}

	// Partition into a neutral family plus hue slices, then give each family
	// slots in proportion to its voxel count. That is how a full-model import
	// gets a pink ramp, a bark ramp, and a grass ramp instead of 128 mixed muds.
	static const int HueSlices = 16;
	static const int FamilyCount = HueSlices + 1;
	static const float NeutralSat = 0.10f;

	core::DynamicArray<WeightedBin> families[FamilyCount];
	uint32_t familyN[FamilyCount];
	for (int i = 0; i < FamilyCount; ++i) {
		familyN[i] = 0;
	}
	for (int i = 0; i < (int)bins.size(); ++i) {
		float h = 0.0f;
		float s = 0.0f;
		float v = 0.0f;
		color::getHSB(bins[i].c, h, s, v);
		int family = 0;
		if (s >= NeutralSat) {
			int slice = (int)(h / (360.0f / (float)HueSlices));
			if (slice < 0) {
				slice = 0;
			}
			if (slice >= HueSlices) {
				slice = HueSlices - 1;
			}
			family = slice + 1;
		}
		WeightedBin wb;
		wb.c = bins[i].c;
		wb.n = bins[i].n;
		families[family].push_back(wb);
		familyN[family] += bins[i].n;
	}

	int slots[FamilyCount];
	int assigned = 0;
	for (int i = 0; i < FamilyCount; ++i) {
		slots[i] = 0;
		if (familyN[i] == 0) {
			continue;
		}
		slots[i] = (int)(((uint64_t)maxColors * (uint64_t)familyN[i] + (total / 2u)) / (uint64_t)total);
		if (slots[i] < 1) {
			slots[i] = 1;
		}
		assigned += slots[i];
	}
	while (assigned > maxColors) {
		int victim = -1;
		for (int i = 0; i < FamilyCount; ++i) {
			if (slots[i] <= 1) {
				continue;
			}
			if (victim < 0 || familyN[i] < familyN[victim]) {
				victim = i;
			}
		}
		if (victim < 0) {
			for (int i = 0; i < FamilyCount; ++i) {
				if (slots[i] != 1) {
					continue;
				}
				if (victim < 0 || familyN[i] < familyN[victim]) {
					victim = i;
				}
			}
		}
		if (victim < 0) {
			break;
		}
		--slots[victim];
		--assigned;
	}
	while (assigned < maxColors) {
		int grow = -1;
		uint32_t growN = 0;
		for (int i = 0; i < FamilyCount; ++i) {
			if (familyN[i] > growN && slots[i] < (int)families[i].size()) {
				growN = familyN[i];
				grow = i;
			}
		}
		if (grow < 0) {
			break;
		}
		++slots[grow];
		++assigned;
	}

	core::DynamicArray<color::RGBA> outColors;
	outColors.reserve(maxColors);
	for (int i = 0; i < FamilyCount; ++i) {
		if (slots[i] <= 0 || families[i].empty()) {
			continue;
		}
		quantizeBinsToRamps(families[i], slots[i], outColors);
	}
	if (outColors.empty()) {
		return palette;
	}
	if ((int)outColors.size() > maxColors) {
		outColors.resize(maxColors);
	}

	outColors.sort([](const color::RGBA &a, const color::RGBA &b) {
		float ha, sa, va, hb, sb, vb;
		color::getHSB(a, ha, sa, va);
		color::getHSB(b, hb, sb, vb);
		if (sa < NeutralSat && sb < NeutralSat) {
			return va < vb;
		}
		if (sa < NeutralSat) {
			return false;
		}
		if (sb < NeutralSat) {
			return true;
		}
		if (ha != hb) {
			return ha > hb;
		}
		return va < vb;
	});

	palette.setSize((int)outColors.size());
	for (int i = 0; i < (int)outColors.size(); ++i) {
		palette.setColor(i, outColors[i]);
	}
	Log::info("Loaded %i unique colors from %i voxels and quantized to %i (hue ramps)", (int)bins.size(), (int)total,
			  palette.colorCount());
	palette.markDirty();
	return palette;
}

#if 0
	struct BoxUnused {
		core::DynamicArray<int> idx;
		uint32_t n = 0;
		uint8_t minR = 255, maxR = 0;
		uint8_t minG = 255, maxG = 0;
		uint8_t minB = 255, maxB = 0;

		void add(int binIndex, const Bin &bin) {
			idx.push_back(binIndex);
			n += bin.n;
			const uint8_t ar = bin.avgR();
			const uint8_t ag = bin.avgG();
			const uint8_t ab = bin.avgB();
			if (ar < minR) {
				minR = ar;
			}
			if (ar > maxR) {
				maxR = ar;
			}
			if (ag < minG) {
				minG = ag;
			}
			if (ag > maxG) {
				maxG = ag;
			}
			if (ab < minB) {
				minB = ab;
			}
			if (ab > maxB) {
				maxB = ab;
			}
		}

		int rangeR() const {
			return (int)maxR - (int)minR;
		}
		int rangeG() const {
			return (int)maxG - (int)minG;
		}
		int rangeB() const {
			return (int)maxB - (int)minB;
		}
		bool canSplit() const {
			return idx.size() > 1 && (rangeR() > 0 || rangeG() > 0 || rangeB() > 0);
		}
		uint64_t importance() const {
			const int dr = rangeR();
			const int dg = rangeG();
			const int db = rangeB();
			return (uint64_t)n * (uint64_t)(dr + dg + db);
		}
	};

	core::DynamicArray<Box> boxes;
	boxes.reserve(maxColors);
	{
		Box root;
		root.idx.reserve((int)bins.size());
		for (int i = 0; i < (int)bins.size(); ++i) {
			root.add(i, bins[i]);
		}
		boxes.push_back(core::move(root));
	}

	while ((int)boxes.size() < maxColors) {
		int best = -1;
		uint64_t bestImp = 0;
		for (int i = 0; i < (int)boxes.size(); ++i) {
			if (!boxes[i].canSplit()) {
				continue;
			}
			const uint64_t imp = boxes[i].importance();
			if (best < 0 || imp > bestImp) {
				bestImp = imp;
				best = i;
			}
		}
		if (best < 0) {
			break;
		}

		Box src = core::move(boxes[best]);
		boxes.erase(best);

		int channel = 0;
		int longest = src.rangeR();
		if (src.rangeG() > longest) {
			channel = 1;
			longest = src.rangeG();
		}
		if (src.rangeB() > longest) {
			channel = 2;
		}

		src.idx.sort([&](int ia, int ib) {
			const Bin &a = bins[ia];
			const Bin &b = bins[ib];
			uint8_t va;
			uint8_t vb;
			if (channel == 0) {
				va = a.avgR();
				vb = b.avgR();
			} else if (channel == 1) {
				va = a.avgG();
				vb = b.avgG();
			} else {
				va = a.avgB();
				vb = b.avgB();
			}
			return va > vb;
		});

		auto channelVal = [&](int binIndex) -> uint8_t {
			if (channel == 0) {
				return bins[binIndex].avgR();
			}
			if (channel == 1) {
				return bins[binIndex].avgG();
			}
			return bins[binIndex].avgB();
		};

		// Split at the largest gap, weighted so both sides keep votes.
		// A population median can glue a small material to a large one.
		int split = 1;
		uint64_t bestScore = 0;
		uint32_t acc = 0;
		for (int i = 0; i < (int)src.idx.size() - 1; ++i) {
			acc += bins[src.idx[i]].n;
			const int gap = (int)channelVal(src.idx[i + 1]) - (int)channelVal(src.idx[i]);
			if (gap <= 0) {
				continue;
			}
			const uint32_t leftN = acc;
			const uint32_t rightN = src.n - acc;
			if (leftN == 0 || rightN == 0) {
				continue;
			}
			const uint32_t smaller = leftN < rightN ? leftN : rightN;
			const uint64_t score = (uint64_t)gap * (uint64_t)smaller;
			if (score > bestScore) {
				bestScore = score;
				split = i + 1;
			}
		}
		if (split <= 0) {
			split = 1;
		}
		if (split >= (int)src.idx.size()) {
			split = (int)src.idx.size() - 1;
		}

		Box left;
		Box right;
		left.idx.reserve(split);
		right.idx.reserve((int)src.idx.size() - split);
		for (int i = 0; i < split; ++i) {
			left.add(src.idx[i], bins[src.idx[i]]);
		}
		for (int i = split; i < (int)src.idx.size(); ++i) {
			right.add(src.idx[i], bins[src.idx[i]]);
		}
		if (left.n == 0 || right.n == 0) {
			boxes.push_back(core::move(src));
			break;
		}
		boxes.push_back(core::move(left));
		boxes.push_back(core::move(right));
	}

	palette.setSize((int)boxes.size());
	for (int i = 0; i < (int)boxes.size(); ++i) {
		const Box &box = boxes[i];
		int best = -1;
		uint32_t bestN = 0;
		for (int j = 0; j < (int)box.idx.size(); ++j) {
			const Bin &bin = bins[box.idx[j]];
			if (best < 0 || bin.n > bestN) {
				best = box.idx[j];
				bestN = bin.n;
			}
		}
		if (best < 0 || bins[best].n == 0) {
			palette.setColor(i, color::RGBA(0, 0, 0, 255));
			continue;
		}
		palette.setColor(i, bins[best].color());
	}
	Log::info("Loaded %i unique colors from %i voxels and quantized to %i (weighted median-cut)", (int)bins.size(),
			  (int)total, palette.colorCount());
	palette.markDirty();
	return palette;
}
#endif

palette::ColorPalette toColorPalette(const palette::Palette &palette) {
	palette::ColorPalette colorPalette;
	colorPalette.setSize(palette.size());
	colorPalette.setName(palette.name());
	colorPalette.setFilename(palette.filename());
	for (size_t i = 0; i < palette.size(); ++i) {
		colorPalette.set(i, palette.color(i), palette.colorName(i), palette.material(i));
	}
	colorPalette.markDirty();
	return colorPalette;
}

} // namespace palette
