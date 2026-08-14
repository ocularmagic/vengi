/**
 * @file
 */

#include "voxelformat/FormatThumbnail.h"
#include "PNGFormat.h"
#include "app/Async.h"
#include "core/ConfigVar.h"
#include "core/Log.h"
#include "core/ScopedPtr.h"
#include "core/StringUtil.h"
#include "core/Var.h"
#include "core/collection/DynamicArray.h"
#include "core/collection/Set.h"
#include "image/Image.h"
#include "io/Archive.h"
#include "io/FilesystemEntry.h"
#include "io/FormatDescription.h"
#include "io/Stream.h"
#include "palette/Palette.h"
#include "palette/PaletteLookup.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "voxel/RawVolume.h"
#include "voxel/RawVolumeWrapper.h"
#include "voxel/Voxel.h"
#include "voxelutil/ImageUtils.h"

namespace voxelformat {

#define MaxHeightmapWidth 4096
#define MaxHeightmapHeight 4096

namespace {

bool isExactWhite(color::RGBA c) {
	return c.r == 255 && c.g == 255 && c.b == 255 && c.a == 255;
}

bool isBuriedSolid(const voxel::RawVolume *volume, int x, int y, int z) {
	if (voxel::isAir(volume->voxel(x + 1, y, z).getMaterial()) ||
		voxel::isAir(volume->voxel(x - 1, y, z).getMaterial()) ||
		voxel::isAir(volume->voxel(x, y + 1, z).getMaterial()) ||
		voxel::isAir(volume->voxel(x, y - 1, z).getMaterial()) ||
		voxel::isAir(volume->voxel(x, y, z + 1).getMaterial()) ||
		voxel::isAir(volume->voxel(x, y, z - 1).getMaterial())) {
		return false;
	}
	return true;
}

int slicePixelIndex(const voxel::Region &region, int x, int z) {
	return (region.getUpperZ() - z) * region.getWidthInVoxels() + (x - region.getLowerX());
}

} // namespace

static int extractLayerFromFilename(const core::String &filename) {
	core::String name = core::string::extractFilename(filename);
	size_t sep = name.rfind('-');
	if (sep == core::String::npos) {
		Log::error("Invalid image name %s", name.c_str());
		return -1;
	}
	const int layer = name.substr(sep + 1).toInt();
	return layer;
}

static bool hasSameBasename(const core::String &originalFilename, const core::String &layerFilename) {
	core::String o = core::string::extractFilename(originalFilename);
	size_t n = o.rfind("-");
	if (n == core::String::npos) {
		return false;
	}
	o = o.substr(0, n);
	core::String l = core::string::extractFilename(layerFilename);
	n = l.rfind("-");
	if (n == core::String::npos) {
		return false;
	}
	l = l.substr(0, n);
	return core::string::iequals(l, o);
}

bool PNGFormat::importSlices(scenegraph::SceneGraph &sceneGraph, const palette::Palette &palette,
							 const io::ArchiveFiles &entities) const {
	const core::String filename = entities.front().fullPath;
	Log::debug("Use %s as reference image", filename.c_str());
	image::ImagePtr referenceImage = image::loadImage(filename);
	if (!referenceImage || !referenceImage->isLoaded()) {
		Log::error("Failed to load first image as reference %s", filename.c_str());
		return false;
	}
	const int imageWidth = referenceImage->width();
	const int imageHeight = referenceImage->height();
	referenceImage.release();
	int minsY = 1000000;
	int maxsY = -1000000;

	core::DynamicArray<const io::FilesystemEntry*> filteredEntites;
	filteredEntites.reserve(entities.size());
	for (const auto &entity : entities) {
		const core::String &layerFilename = entity.fullPath;
		if (!hasSameBasename(filename, layerFilename)) {
			continue;
		}
		if (!io::isImage(layerFilename)) {
			continue;
		}
		const int layer = extractLayerFromFilename(layerFilename);
		if (layer < 0) {
			Log::error("Failed to extract layer from filename %s", layerFilename.c_str());
			continue;
		}
		minsY = glm::min(minsY, layer);
		maxsY = glm::max(maxsY, layer);
		filteredEntites.push_back(&entity);
	}

	// Image is an XZ top-down cut; the filename layer is world Y.
	voxel::Region region(0, minsY, 0, imageWidth - 1, maxsY, imageHeight - 1);
	voxel::RawVolume *volume = new voxel::RawVolume(region);
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	node.setVolume(volume);
	node.setName(core::string::extractFilename(filename));
	node.setPalette(palette);

	palette::PaletteLookup palLookup(palette);
	auto fn = [&filteredEntites, &palLookup, &palette, &volume, imageHeight, imageWidth, this] (int start, int end) {
		for (int i = start; i < end; ++i) {
			const auto &entity = *filteredEntites[i];
			const core::String &layerFilename = entity.fullPath;
			const image::ImagePtr &image = image::loadImage(layerFilename);
			if (!image || !image->isLoaded()) {
				Log::error("Failed to load image %s", layerFilename.c_str());
				continue;
			}
			if (imageWidth != image->width() || imageHeight != image->height()) {
				Log::error("Image %s has different dimensions than the first image (%d:%d) vs (%d:%d)",
						layerFilename.c_str(), image->width(), image->height(), imageWidth, imageHeight);
				continue;
			}
			const int layer = extractLayerFromFilename(layerFilename);
			if (layer < 0) {
				Log::error("Failed to extract layer from filename %s", layerFilename.c_str());
				continue;
			}
			Log::debug("Import layer %i of image %s", layer, layerFilename.c_str());

			voxel::RawVolume::Sampler sampler(volume);
			sampler.setPosition(0, layer, imageHeight - 1);
			for (int y = 0; y < imageHeight; ++y) {
				voxel::RawVolume::Sampler sampler2 = sampler;
				for (int x = 0; x < imageWidth; ++x) {
					const color::RGBA &color = flattenRGB(image->colorAt(x, y));
					if (color.a == 0) {
						sampler2.movePositiveX();
						continue;
					}
					const int palIdx = palLookup.findClosestIndex(color);
					sampler2.setVoxel(voxel::createVoxel(palette, palIdx));
					sampler2.movePositiveX();
				}
				sampler.moveNegativeZ();
			}
		}
	};
	app::for_parallel(0, filteredEntites.size(), fn);
	if (sceneGraph.emplace(core::move(node)) == InvalidNodeId) {
		Log::error("Failed to add node to scene graph");
		return false;
	}
	return true;
}

bool PNGFormat::importAsHeightmap(scenegraph::SceneGraph &sceneGraph, const palette::Palette &palette,
								  const core::String &filename, const io::ArchivePtr &archive) const {
	image::ImagePtr image = image::loadImage(filename);
	if (image->width() > MaxHeightmapWidth || image->height() >= MaxHeightmapHeight) {
		Log::warn("Skip creating heightmap - image dimensions exceeds the max allowed boundaries");
		return false;
	}
	const bool coloredHeightmap = image->components() == 4 && !image->isGrayScale();
	const int maxHeight = voxelutil::importHeightMaxHeight(image, coloredHeightmap);
	if (maxHeight <= 0) {
		Log::error("There is no height in either the red channel or the alpha channel");
		return false;
	}
	if (maxHeight == 1) {
		Log::warn("There is no height value in the image - it is imported as flat plane");
	}
	Log::info("Generate from heightmap (%i:%i) with max height of %i", image->width(), image->height(), maxHeight);
	voxel::Region region(0, 0, 0, image->width() - 1, maxHeight - 1, image->height() - 1);
	voxel::RawVolume *volume = new voxel::RawVolume(region);
	voxel::RawVolumeWrapper wrapper(volume);
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	const voxel::Voxel dirtVoxel = voxel::createVoxel(voxel::VoxelType::Generic, 1);
	const uint8_t minHeight = core::getVar(cfg::VoxformatImageHeightmapMinHeight)->intVal();
	if (coloredHeightmap) {
		voxelutil::importColoredHeightmap(wrapper, palette, image, dirtVoxel, minHeight, false);
	} else {
		const voxel::Voxel grassVoxel = voxel::createVoxel(voxel::VoxelType::Generic, 2);
		voxelutil::importHeightmap(wrapper, image, dirtVoxel, grassVoxel, minHeight, false);
	}
	node.setPalette(palette);
	node.setVolume(volume);
	node.setName(core::string::extractFilename(filename));
	return sceneGraph.emplace(core::move(node)) != InvalidNodeId;
}

bool PNGFormat::importAsVolume(scenegraph::SceneGraph &sceneGraph, const palette::Palette &palette,
							   const core::String &filename, const io::ArchivePtr &archive) const {
	const image::ImagePtr &image = image::loadImage(filename);
	const int maxDepth = core::getVar(cfg::VoxformatImageVolumeMaxDepth)->intVal();
	const bool bothSides = core::getVar(cfg::VoxformatImageVolumeBothSides)->boolVal();
	const core::String &depthMapFilename = voxelutil::getDefaultDepthMapFile(filename);
	core::ScopedPtr<io::SeekableReadStream> depthMapStream(archive->readStream(depthMapFilename));
	const image::ImagePtr &depthMapImage = image::loadImage(depthMapFilename, *depthMapStream, depthMapStream->size());
	voxel::RawVolume *v;
	if (depthMapImage && depthMapImage->isLoaded()) {
		Log::debug("Found depth map %s", depthMapFilename.c_str());
		v = voxelutil::importAsVolume(image, depthMapImage, palette, maxDepth, bothSides);
	} else {
		Log::debug("Could not find a depth map for %s with the name %s", filename.c_str(), depthMapFilename.c_str());
		v = voxelutil::importAsVolume(image, palette, maxDepth, bothSides);
	}
	if (v == nullptr) {
		Log::warn("Failed to import image as volume: '%s'", image->name().c_str());
		return false;
	}
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	node.setVolume(v);
	node.setName(core::string::extractFilename(filename));
	node.setPalette(palette);
	return sceneGraph.emplace(core::move(node)) != InvalidNodeId;
}

bool PNGFormat::importAsPlane(scenegraph::SceneGraph &sceneGraph, const palette::Palette &palette,
							  const core::String &filename, const io::ArchivePtr &archive) const {
	image::ImagePtr image = image::loadImage(filename);
	voxel::RawVolume *v = voxelutil::importAsPlane(image, palette);
	if (v == nullptr) {
		Log::warn("Failed to import image as plane: '%s'", image->name().c_str());
		return false;
	}
	scenegraph::SceneGraphNode node(scenegraph::SceneGraphNodeType::Model);
	node.setVolume(v);
	node.setName(core::string::extractFilename(filename));
	node.setPalette(palette);
	return sceneGraph.emplace(core::move(node)) != InvalidNodeId;
}

bool PNGFormat::loadGroupsRGBA(const core::String &filename, const io::ArchivePtr &archive,
							   scenegraph::SceneGraph &sceneGraph, const palette::Palette &palette,
							   const LoadContext &ctx) {
	const int type = core::getVar(cfg::VoxformatImageImportType)->intVal();
	if (type == ImageType::Heightmap) {
		return importAsHeightmap(sceneGraph, palette, filename, archive);
	}
	if (type == ImageType::Volume) {
		return importAsVolume(sceneGraph, palette, filename, archive);
	}

	core::String basename = core::string::extractFilename(filename);
	const core::String &directory = core::string::extractDir(filename);
	size_t sep = basename.rfind('-');
	if (sep != core::String::npos) {
		basename = basename.substr(0, sep);
	}
	Log::debug("Base name for image layer import is: %s", basename.c_str());

	io::ArchiveFiles entities;
	archive->list(directory, entities, core::String::format("%s-*.png", basename.c_str()));
	if (entities.empty()) {
		io::FilesystemEntry val = io::createFilesystemEntry(filename);
		entities.push_back(val);
	}
	Log::debug("Found %i images for import", (int)entities.size());

	if (entities.size() > 1u) {
		return importSlices(sceneGraph, palette, entities);
	}
	return importAsPlane(sceneGraph, palette, filename, archive);
}

size_t PNGFormat::loadPalette(const core::String &filename, const io::ArchivePtr &archive, palette::Palette &palette,
							  const LoadContext &ctx) {
	const image::ImagePtr &image = image::loadImage(filename);
	const int type = core::getVar(cfg::VoxformatImageImportType)->intVal();
	if (type == ImageType::Heightmap) {
		image->makeOpaque();
	}

	if (image && image->isLoaded()) {
		if (palette.createPalette(image, palette)) {
			Log::debug("Created palette with %i colors from image %s", palette.colorCount(), filename.c_str());
			return palette.colorCount();
		}
	}

	// if the main file doesn't exist, try to find slice images and collect all colors
	core::String basename = core::string::extractFilename(filename);
	const core::String &directory = core::string::extractDir(filename);
	const size_t sep = basename.rfind('-');
	if (sep != core::String::npos) {
		basename = basename.substr(0, sep);
	}
	io::ArchiveFiles entities;
	archive->list(directory, entities, core::String::format("%s-*.png", basename.c_str()));
	core::Set<color::RGBA, 521> colorSet;
	for (const auto &entity : entities) {
		const image::ImagePtr &sliceImage = image::loadImage(entity.fullPath);
		if (!sliceImage || !sliceImage->isLoaded()) {
			continue;
		}
		for (int x = 0; x < sliceImage->width(); ++x) {
			for (int y = 0; y < sliceImage->height(); ++y) {
				const color::RGBA c = sliceImage->colorAt(x, y);
				if (c.a > 0) {
					colorSet.insert(c);
				}
			}
		}
	}
	if (!colorSet.empty()) {
		core::Buffer<color::RGBA> colors;
		colors.reserve(colorSet.size());
		for (const auto &e : colorSet) {
			colors.push_back(e->first);
		}
		palette.quantize(colors.data(), colors.size());
		palette.markDirty();
		Log::debug("Created palette with %i colors from %i slices", palette.colorCount(), (int)entities.size());
		return palette.colorCount();
	}

	Log::error("Failed to create palette from image %s", filename.c_str());
	return 0;
}

bool PNGFormat::saveThumbnail(const scenegraph::SceneGraph &sceneGraph, const core::String &filename,
							   const io::ArchivePtr &archive, const SaveContext &savectx) const {
	Log::debug("Create thumbnail for %s", filename.c_str());
	ThumbnailContext ctx;
	// if we are using the default thumbnailer, we want it to be pixel perfect - and here we
	// use the internal knowledge that -1 simply prevents a scaling of the resulting image.
	if (savectx.thumbnailCreator == nullptr) {
		ctx.outputSize = {-1, -1};
	}
	const image::ImagePtr &image = createThumbnail(sceneGraph, savectx.thumbnailCreator, ctx);
	if (!image || !image->isLoaded()) {
		Log::error("Failed to create thumbnail for %s", filename.c_str());
		return false;
	}
	core::ScopedPtr<io::SeekableWriteStream> writeStream(archive->writeStream(filename));
	if (!writeStream) {
		Log::error("Failed to open write stream for %s", filename.c_str());
		return false;
	}
	if (!image->writePNG(*writeStream)) {
		Log::error("Failed to write slice image %s", filename.c_str());
		return false;
	}
	return true;
}

bool PNGFormat::saveGroups(const scenegraph::SceneGraph &sceneGraph, const core::String &filename,
						   const io::ArchivePtr &archive, const SaveContext &ctx) {
	const int type = core::getVar(cfg::VoxformatImageSaveType)->intVal();
	if (type == ImageType::Heightmap) {
		return saveHeightmaps(sceneGraph, filename, archive);
	}
	if (type == ImageType::Volume) {
		return saveVolumes(sceneGraph, filename, archive);
	}
	if (type == ImageType::Thumbnail) {
		return saveThumbnail(sceneGraph, filename, archive, ctx);
	}

	return saveSlices(sceneGraph, filename, archive);
}

bool PNGFormat::saveHeightmaps(const scenegraph::SceneGraph &sceneGraph, const core::String &filename,
							   const io::ArchivePtr &archive) const {
	for (const auto &e : sceneGraph.nodes()) {
		const scenegraph::SceneGraphNode &node = e->value;
		if (!node.isAnyModelNode()) {
			continue;
		}
		const voxel::RawVolume *volume = sceneGraph.resolveVolume(node);
		core_assert(volume != nullptr);
		const voxel::Region &region = volume->region();
		// TODO: VOXELFORMAT: make max height configurable
		const float heightScale = 256.0f / (float)region.getHeightInVoxels();
		const palette::Palette &palette = node.palette();
		const core::String &uuidStr = node.uuid().str();
		const core::String name = core::String::format("%s-%s.png", core::string::stripExtension(filename).c_str(),
												 uuidStr.c_str());
		image::Image image(name, 4);
		image.resize(region.getWidthInVoxels(), region.getDepthInVoxels());
		app::for_parallel(region.getLowerZ(), region.getUpperZ() + 1, [&image, volume, &region, &palette, heightScale] (int start, int end) {
			voxel::RawVolume::Sampler sampler(volume);
			sampler.setPosition(region.getLowerX(), region.getUpperY(), start);
			for (int z = start; z < end; ++z) {
				voxel::RawVolume::Sampler sampler2 = sampler;
				for (int x = region.getLowerX(); x <= region.getUpperX(); ++x) {
					voxel::RawVolume::Sampler sampler3 = sampler2;
					for (int y = region.getUpperY(); y >= region.getLowerY(); --y) {
						if (isBlocked(sampler3.voxel().getMaterial())) {
							color::RGBA color = palette.color(sampler3.voxel().getColor());
							color.a = (y + 1) * heightScale;
							image.setColor(color, x - region.getLowerX(),
										z - region.getLowerZ());
							break;
						}
						sampler3.moveNegativeY();
					}
					sampler2.movePositiveX();
				}
				sampler.movePositiveZ();
			}
		});
		core::ScopedPtr<io::SeekableWriteStream> writeStream(archive->writeStream(name));
		if (!writeStream) {
			Log::error("Failed to open write stream for %s", name.c_str());
			return false;
		}
		if (!image.writePNG(*writeStream)) {
			Log::error("Failed to write image %s", name.c_str());
			return false;
		}
		Log::debug("Saved heightmap image %s", name.c_str());
	}
	return true;
}

bool PNGFormat::saveVolumes(const scenegraph::SceneGraph &sceneGraph, const core::String &filename,
							const io::ArchivePtr &archive) const {
	Log::error("Saving volumes as PNG is not supported");
	return false;
}

bool PNGFormat::saveSlices(const scenegraph::SceneGraph &sceneGraph, const core::String &filename,
						   const io::ArchivePtr &archive) const {
	const bool hollowInterior = core::getVar(cfg::VoxformatImageSliceHollowInterior)->boolVal();
	const core::String &basename = core::string::stripExtension(filename);
	for (const auto &e : sceneGraph.nodes()) {
		const scenegraph::SceneGraphNode &node = e->value;
		if (!node.isAnyModelNode()) {
			continue;
		}
		const voxel::RawVolume *volume = sceneGraph.resolveVolume(node);
		core_assert(volume != nullptr);
		const voxel::Region &region = volume->region();
		const palette::Palette &palette = node.palette();
		const int width = region.getWidthInVoxels();
		const int height = region.getDepthInVoxels();
		// Top to bottom: one XZ image per Y. First file written is the top of the model.
		for (int y = region.getUpperY(); y >= region.getLowerY(); --y) {
			const core::String &uuidStr = node.uuid().str();
			const core::String &layerFilename =
				core::String::format("%s-%s-%i.png", basename.c_str(), uuidStr.c_str(), y);
			image::Image image(layerFilename);
			core::Buffer<color::RGBA> rgba;
			rgba.resize(width * height);
			core::Buffer<uint8_t> buriedWhite;
			if (hollowInterior) {
				buriedWhite.resize(width * height);
			}
			bool empty = true;
			for (int z = region.getUpperZ(); z >= region.getLowerZ(); --z) {
				for (int x = region.getLowerX(); x <= region.getUpperX(); ++x) {
					const voxel::Voxel &v = volume->voxel(x, y, z);
					if (voxel::isAir(v.getMaterial())) {
						continue;
					}
					const color::RGBA color = palette.color(v.getColor());
					const int idx = slicePixelIndex(region, x, z);
					rgba[idx] = color;
					empty = false;
					if (hollowInterior && isExactWhite(color) && isBuriedSolid(volume, x, y, z)) {
						buriedWhite[idx] = 1;
					}
				}
			}
			if (hollowInterior && !empty) {
				static const int ndx[4] = {1, -1, 0, 0};
				static const int ndz[4] = {0, 0, 1, -1};
				for (int z = region.getLowerZ(); z <= region.getUpperZ(); ++z) {
					for (int x = region.getLowerX(); x <= region.getUpperX(); ++x) {
						const int idx = slicePixelIndex(region, x, z);
						if (!buriedWhite[idx]) {
							continue;
						}
						bool keepLiner = false;
						for (int i = 0; i < 4; ++i) {
							const int nx = x + ndx[i];
							const int nz = z + ndz[i];
							if (!region.containsPoint(nx, y, nz)) {
								continue;
							}
							const color::RGBA neighbor = rgba[slicePixelIndex(region, nx, nz)];
							if (neighbor.a == 0) {
								continue;
							}
							if (!isExactWhite(neighbor)) {
								keepLiner = true;
								break;
							}
						}
						if (!keepLiner) {
							rgba[idx] = color::RGBA(0, 0, 0, 0);
						}
					}
				}
				empty = true;
				for (int i = 0; i < width * height; ++i) {
					if (rgba[i].a != 0) {
						empty = false;
						break;
					}
				}
			}
			if (empty) {
				// skip empty slices
				continue;
			}

			if (!image.loadRGBA((const uint8_t *)rgba.data(), width, height)) {
				Log::error("Failed to load sliced rgba data %s", layerFilename.c_str());
				return false;
			}
			core::ScopedPtr<io::SeekableWriteStream> writeStream(archive->writeStream(layerFilename));
			if (!writeStream) {
				Log::error("Failed to open write stream for %s", layerFilename.c_str());
				return false;
			}
			if (!image.writePNG(*writeStream)) {
				Log::error("Failed to write slice image %s", layerFilename.c_str());
				return false;
			}
		}
	}
	return true;
}

} // namespace voxelformat
