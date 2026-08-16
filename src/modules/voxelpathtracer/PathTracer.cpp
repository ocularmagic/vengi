/**
 * @file
 */

#include "color/ColorUtil.h"
#include "PathTracer.h"
#include "color/Color.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "core/ConfigVar.h"
#include "core/Var.h"
#include "image/Image.h"
#include "io/File.h"
#include "io/Stream.h"
#include "palette/Palette.h"
#include "palette/PaletteView.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "scenegraph/SceneGraphNodeProperties.h"
#include "video/Camera.h"
#include "core/GLM.h"
#include "voxel/ChunkMesh.h"
#include "voxel/Mesh.h"
#include "voxel/RawVolume.h"
#include "voxel/SurfaceExtractor.h"
#include "voxelrender/RenderUtil.h"
#include "PathTracerState.h"

#include <stb_image.h>

#define PATHTRACER_TEXTURES 0

namespace voxelpathtracer {

namespace priv {

static inline yocto::vec3f toVec3f(const glm::vec3 &in) {
	return yocto::vec3f{in.x, in.y, in.z};
}

static inline yocto::vec4f toColor(const glm::vec4 &in, float ambientOcclusion_unused) {
	return yocto::vec4f{in.r, in.g, in.b, in.a};
}

/**
 * Simplified read stream that knows how image::Image::loadRGBA() works.
 *
 * @code
 * YoctoImageReadStream stream(img);
 * target.loadRGBA(stream, img.width, img.height);
 * @endcode
 */
class YoctoImageReadStream : public io::ReadStream {
private:
	const yocto::image_data &_img;
	bool _eos = false;

public:
	YoctoImageReadStream(const yocto::image_data &img) : _img(img) {
	}

	// the complete image is read with one call!!
	int read(void *dataPtr, size_t dataSize) override {
		const size_t actualSize =  (size_t)_img.width * (size_t)_img.height * sizeof(yocto::vec4b);
		if (dataSize != actualSize) {
			Log::error("Expected to read %d bytes, but got %d", (int)actualSize, (int)dataSize);
			return -1;
		}
		uint8_t *buf = (uint8_t *)dataPtr;
		for (int i = 0; i < _img.height; i++) {
			for (int j = 0; j < _img.width; j++) {
				const yocto::vec4b &v = yocto::float_to_byte(_img[{j, i}]);
				memcpy(buf, &v, sizeof(v));
				buf += sizeof(v);
			}
		}
		_eos = true;
		Log::debug("Loaded %d bytes from the image with size %dx%d", (int)dataSize, _img.width, _img.height);
		return (int)dataSize;
	}

	bool eos() const override {
		return _eos;
	}
};

static void pushTriangle(yocto::shape_data &shape, const yocto::vec3f &p0, const yocto::vec3f &p1,
						 const yocto::vec3f &p2, const yocto::vec4f &color) {
	const int offsetStart = (int)shape.triangles.size() * 3;
	shape.positions.push_back(p0);
	shape.positions.push_back(p1);
	shape.positions.push_back(p2);
	shape.colors.push_back(color);
	shape.colors.push_back(color);
	shape.colors.push_back(color);
	shape.triangles.push_back({offsetStart + 0, offsetStart + 1, offsetStart + 2});
}

static void pushBevelQuad(yocto::shape_data &shape, const yocto::vec3f &c0, const yocto::vec3f &c1,
						  const yocto::vec3f &c2, const yocto::vec3f &c3, const yocto::vec4f &color) {
	const yocto::vec3f center = (c0 + c1 + c2 + c3) / 4.0f;
	const float inset = 0.03f;
	const yocto::vec3f i0 = c0 + (center - c0) * inset;
	const yocto::vec3f i1 = c1 + (center - c1) * inset;
	const yocto::vec3f i2 = c2 + (center - c2) * inset;
	const yocto::vec3f i3 = c3 + (center - c3) * inset;
	pushTriangle(shape, i0, i1, i2, color);
	pushTriangle(shape, i0, i2, i3, color);
	const yocto::vec4f rim{color.x * 0.68f, color.y * 0.68f, color.z * 0.68f, color.w};
	pushTriangle(shape, c0, c1, i1, rim);
	pushTriangle(shape, c0, i1, i0, rim);
	pushTriangle(shape, c1, c2, i2, rim);
	pushTriangle(shape, c1, i2, i1, rim);
	pushTriangle(shape, c2, c3, i3, rim);
	pushTriangle(shape, c2, i3, i2, rim);
	pushTriangle(shape, c3, c0, i0, rim);
	pushTriangle(shape, c3, i0, i3, rim);
}

static bool loadHdriTexture(const core::String &path, yocto::texture_data &texture) {
	io::File file(path, io::FileMode::SysRead);
	if (!file.exists()) {
		Log::error("HDRI file not found: %s", path.c_str());
		return false;
	}
	void *buffer = nullptr;
	const int len = file.read(&buffer);
	if (len <= 0 || buffer == nullptr) {
		Log::error("Failed to read HDRI file: %s", path.c_str());
		delete[] (uint8_t *)buffer;
		return false;
	}
	int width = 0;
	int height = 0;
	int components = 0;
	float *pixels = stbi_loadf_from_memory((const stbi_uc *)buffer, len, &width, &height, &components, 4);
	delete[] (uint8_t *)buffer;
	if (pixels == nullptr || width <= 0 || height <= 0) {
		Log::error("Failed to decode HDRI %s: %s", path.c_str(), stbi_failure_reason());
		if (pixels != nullptr) {
			stbi_image_free(pixels);
		}
		return false;
	}
	yocto::image_data image = yocto::make_image(width, height, true);
	const int count = width * height;
	for (int i = 0; i < count; ++i) {
		image.pixels[i] = {pixels[i * 4 + 0], pixels[i * 4 + 1], pixels[i * 4 + 2], pixels[i * 4 + 3]};
	}
	stbi_image_free(pixels);
	texture = yocto::image_to_texture(image);
	return true;
}

} // namespace priv

PathTracer::PathTracer() : _state(new PathTracerState()) {
}

PathTracer::~PathTracer() {
	stop();
	delete _state;
}

bool PathTracer::addNode(const scenegraph::SceneGraph &sceneGraph, const scenegraph::SceneGraphNode &node,
						 const voxel::Mesh &mesh, bool opaque) {
	const voxel::IndexArray &indices = mesh.getIndexVector();
	if (indices.empty()) {
		return true;
	}
	core_assert((int)indices.size() % 3 == 0);
	const int tris = (int)indices.size() / 3;
	yocto::shape_data shapes[palette::PaletteMaxColors];
	const voxel::VertexArray &vertices = mesh.getVertexVector();
	const voxel::NormalArray &normals = mesh.getNormalVector();
	const bool useNormals = normals.size() == vertices.size();

	const palette::Palette &palette = sceneGraph.resolvePalette(node);
	scenegraph::KeyFrameIndex keyFrameIdx = 0;
	const scenegraph::SceneGraphTransform &transform = node.transform(keyFrameIdx);
	const voxel::Region &region = sceneGraph.resolveRegion(node);
	const glm::vec3 size(region.getDimensionsInVoxels());
	const glm::vec3 &pivot = node.pivot();
	const glm::vec3 objPivot = pivot * size;
	const bool studioEdges = _state->studioEdges;
	for (int i = 0; i < tris;) {
		const voxel::VoxelVertex &vertex0 = vertices[indices[i * 3 + 0]];
		const voxel::VoxelVertex &vertex1 = vertices[indices[i * 3 + 1]];
		const voxel::VoxelVertex &vertex2 = vertices[indices[i * 3 + 2]];
		yocto::shape_data *shape = &shapes[vertex0.colorIndex];
		const color::RGBA rgba = palette.color(vertex0.colorIndex);
		const yocto::vec4f color = priv::toColor(color::fromRGBA(rgba), vertex0.ambientOcclusion);

		if (studioEdges && i + 1 < tris) {
			const voxel::IndexType i0 = indices[i * 3 + 0];
			const voxel::IndexType i1 = indices[i * 3 + 1];
			const voxel::IndexType i2 = indices[i * 3 + 2];
			const voxel::IndexType i3 = indices[(i + 1) * 3 + 0];
			const voxel::IndexType i4 = indices[(i + 1) * 3 + 1];
			const voxel::IndexType i5 = indices[(i + 1) * 3 + 2];
			voxel::IndexType fourth = i5;
			if (i3 != i0 && i3 != i1 && i3 != i2) {
				fourth = i3;
			} else if (i4 != i0 && i4 != i1 && i4 != i2) {
				fourth = i4;
			}
			const glm::vec3 p0 = transform.apply(vertices[i0].position, objPivot);
			const glm::vec3 p1 = transform.apply(vertices[i1].position, objPivot);
			const glm::vec3 p2 = transform.apply(vertices[i2].position, objPivot);
			const glm::vec3 p3 = transform.apply(vertices[fourth].position, objPivot);
			priv::pushBevelQuad(*shape, priv::toVec3f(p0), priv::toVec3f(p1), priv::toVec3f(p2), priv::toVec3f(p3),
								color);
			(void)useNormals;
			i += 2;
			continue;
		}

		const glm::vec3 pos0 = transform.apply(vertex0.position, objPivot);
		const glm::vec3 pos1 = transform.apply(vertex1.position, objPivot);
		const glm::vec3 pos2 = transform.apply(vertex2.position, objPivot);
		priv::pushTriangle(*shape, priv::toVec3f(pos0), priv::toVec3f(pos1), priv::toVec3f(pos2), color);
		++i;
	}
	// const glm::vec3 &mins = mesh.getOffset();
	_state->scene.shapes.reserve(palette.colorCount());
	for (int i = 0; i < palette.colorCount(); ++i) {
		yocto::shape_data &shape = shapes[i];
		if (shape.triangles.empty()) {
			continue;
		}
		_state->scene.shapes.push_back(shape);

		yocto::instance_data instance_data;
		// instance_data.frame = yocto::translation_frame(priv::toVec3f(mins));
		instance_data.material = _state->scene.materials.size() + i;
		instance_data.shape = (int)_state->scene.shapes.size() - 1;
		_state->scene.instances.push_back(instance_data);
	}

	return true;
}

void PathTracer::applyAppearanceFromScene(const scenegraph::SceneGraph &sceneGraph) {
	_state->hdriEnvironment = false;
	_state->hdriPath = "";
	_state->hdriIntensity = 1.0f;
	_state->hdriAzimuth = 0.0f;
	_state->groundPlane = false;
	_state->studioEdges = false;

	const scenegraph::SceneGraphNode &root = sceneGraph.root();
	const core::String &hdri = root.property(scenegraph::PropHdri);
	if (!hdri.empty()) {
		_state->hdriEnvironment = hdri == "true";
	}
	const core::String &hdriPath = root.property(scenegraph::PropHdriPath);
	if (!hdriPath.empty()) {
		_state->hdriPath = hdriPath;
	}
	const core::String &hdriIntensity = root.property(scenegraph::PropHdriIntensity);
	if (!hdriIntensity.empty()) {
		_state->hdriIntensity = core::string::toFloat(hdriIntensity);
	}
	const core::String &hdriAzimuth = root.property(scenegraph::PropHdriAzimuth);
	if (!hdriAzimuth.empty()) {
		_state->hdriAzimuth = glm::radians(core::string::toFloat(hdriAzimuth));
	}
	const core::String &groundPlane = root.property(scenegraph::PropGroundPlane);
	if (!groundPlane.empty()) {
		_state->groundPlane = groundPlane == "true";
	}
	const core::String &studioEdges = root.property(scenegraph::PropStudioEdges);
	if (!studioEdges.empty()) {
		_state->studioEdges = studioEdges == "true";
	}
}

static bool setFloatPropertyIfChanged(scenegraph::SceneGraphNode &root, const char *key, float value) {
	const core::String &cur = root.property(key);
	if (!cur.empty() && glm::abs(cur.toFloat() - value) <= 0.001f) {
		return false;
	}
	return root.setProperty(key, core::string::toString(value));
}

bool PathTracer::writeAppearanceToScene(const scenegraph::SceneGraph &sceneGraph) const {
	scenegraph::SceneGraphNode &root = sceneGraph.node(sceneGraph.root().id());
	bool changed = false;
	changed |= root.setProperty(scenegraph::PropHdri, _state->hdriEnvironment ? "true" : "false");
	changed |= root.setProperty(scenegraph::PropHdriPath, _state->hdriPath);
	changed |= setFloatPropertyIfChanged(root, scenegraph::PropHdriIntensity, _state->hdriIntensity);
	changed |= setFloatPropertyIfChanged(root, scenegraph::PropHdriAzimuth, glm::degrees(_state->hdriAzimuth));
	changed |= root.setProperty(scenegraph::PropGroundPlane, _state->groundPlane ? "true" : "false");
	changed |= root.setProperty(scenegraph::PropStudioEdges, _state->studioEdges ? "true" : "false");
	return changed;
}

void PathTracer::addGroundPlane(const scenegraph::SceneGraph &) {
	// Sit on the lowest already-emitted mesh vertex. Volume regions often
	// include empty cells under the sculpt, which left a gap under the model.
	bool any = false;
	float minX = 1.0e30f;
	float minY = 1.0e30f;
	float minZ = 1.0e30f;
	float maxX = -1.0e30f;
	float maxZ = -1.0e30f;
	for (const yocto::shape_data &meshShape : _state->scene.shapes) {
		for (const yocto::vec3f &p : meshShape.positions) {
			minX = glm::min(minX, p.x);
			minY = glm::min(minY, p.y);
			minZ = glm::min(minZ, p.z);
			maxX = glm::max(maxX, p.x);
			maxZ = glm::max(maxZ, p.z);
			any = true;
		}
	}
	if (!any) {
		return;
	}
	const float spanX = maxX - minX;
	const float spanZ = maxZ - minZ;
	const float pad = glm::max(16.0f, glm::max(spanX, spanZ));
	const float y = minY;
	const float x0 = minX - pad;
	const float x1 = maxX + pad;
	const float z0 = minZ - pad;
	const float z1 = maxZ + pad;

	yocto::shape_data shape;
	priv::pushTriangle(shape, {x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {0.82f, 0.82f, 0.84f, 1.0f});
	priv::pushTriangle(shape, {x0, y, z0}, {x1, y, z1}, {x0, y, z1}, {0.82f, 0.82f, 0.84f, 1.0f});
	_state->scene.shapes.push_back(shape);

	yocto::material_data material;
	material.type = yocto::material_type::matte;
	material.color = {0.82f, 0.82f, 0.84f};
	_state->scene.materials.push_back(material);

	yocto::instance_data instance;
	instance.shape = (int)_state->scene.shapes.size() - 1;
	instance.material = (int)_state->scene.materials.size() - 1;
	_state->scene.instances.push_back(instance);
}

void PathTracer::addCamera(const scenegraph::SceneGraphNodeCamera &node) {
	addCamera(node.name().c_str(), voxelrender::toCamera({}, node));
}

void PathTracer::addCamera(const char *name, const video::Camera &cam) {
	yocto::scene_data &scene = _state->scene;
	scene.camera_names.emplace_back(name);
	yocto::camera_data &camera = scene.cameras.emplace_back();

	// Copy the editor view matrix. Reconstructing with lookat(eye, target)
	// misses the real orientation when the orbit pivot is not along the view
	// axis, which makes the path tracer sit at a flatter angle than the viewport.
	video::Camera updated = cam;
	updated.update(0.0);
	const glm::mat4 &invView = updated.inverseViewMatrix();
	camera.frame.x = priv::toVec3f(glm::vec3(invView[0]));
	camera.frame.y = priv::toVec3f(glm::vec3(invView[1]));
	camera.frame.z = priv::toVec3f(glm::vec3(invView[2]));
	camera.frame.o = priv::toVec3f(glm::vec3(invView[3]));

	const glm::ivec2 size = glm::max(updated.size(), glm::ivec2(1, 1));
	// Yocto wants width/height. video::Camera::aspect() is inverted for perspective.
	camera.aspect = (float)size.x / (float)size.y;
	camera.aperture = _state->aperture;

	camera.orthographic = updated.isOrthographic();
	if (camera.orthographic) {
		camera.film = (float)size.x;
		if (updated.rotationType() == video::CameraRotationType::Target) {
			camera.focus = updated.targetDistance();
		} else {
			camera.focus = updated.farPlane();
		}
		camera.lens = camera.film / camera.focus;
	} else {
		camera.film = 0.036f;
		// Match glm::perspectiveFovRH: fieldOfView() is vertical, in degrees.
		// Yocto stores film as the long side; divide by aspect when landscape
		// so the vertical film is film/aspect.
		const float fovRadians = glm::radians(updated.fieldOfView());
		float lens = camera.film / (2.0f * glm::tan(fovRadians / 2.0f));
		if (camera.aspect >= 1.0f) {
			lens /= camera.aspect;
		}
		if (updated.rotationType() == video::CameraRotationType::Target) {
			camera.focus = updated.targetDistance();
		} else {
			camera.focus = updated.farPlane();
		}
		// Pinhole lens. Do not apply the thin-lens focus formula here: that
		// changes FOV and is only needed when aperture > 0.
		camera.lens = lens;
	}
}

static yocto::material_type mapMaterialType(palette::MaterialType type) {
	// https://xelatihy.github.io/yocto-gl/yocto/yocto_scene/#materials
	switch (type) {
	case palette::MaterialType::Diffuse:
		return yocto::material_type::matte;
	case palette::MaterialType::Emit:
		return yocto::material_type::matte;
	case palette::MaterialType::Metal:
		return yocto::material_type::reflective;
	case palette::MaterialType::Glass:
		return yocto::material_type::refractive;
	case palette::MaterialType::Blend:
		return yocto::material_type::transparent;
	case palette::MaterialType::Media:
		return yocto::material_type::volumetric;
	}
	return yocto::material_type::matte;
}

static void setupMaterial(yocto::scene_data &scene, const palette::Palette &palette, int i) {
	const palette::Material &ownMaterial = palette.material(i);

	yocto::material_data material;
	material.type = mapMaterialType(ownMaterial.type);
	const glm::vec4 color = color::fromRGBA(palette.color(i));
	material.color = priv::toVec3f(color);
	if (ownMaterial.has(palette::MaterialProperty::MaterialEmit)) {
		const glm::vec4 emitColor = color::fromRGBA(palette.emitColor(i));
		material.emission = priv::toVec3f(emitColor) * ownMaterial.value(palette::MaterialProperty::MaterialEmit);
	}
	if (ownMaterial.has(palette::MaterialProperty::MaterialMetal)) {
		material.metallic = ownMaterial.value(palette::MaterialProperty::MaterialMetal);
	}
	if (ownMaterial.has(palette::MaterialProperty::MaterialRoughness)) {
		material.roughness = ownMaterial.value(palette::MaterialProperty::MaterialRoughness);
	}
	if (ownMaterial.has(palette::MaterialProperty::MaterialIndexOfRefraction)) {
		material.ior = ownMaterial.value(palette::MaterialProperty::MaterialIndexOfRefraction);
	}
	if (ownMaterial.has(palette::MaterialProperty::MaterialPhase)) {
		material.scanisotropy = ownMaterial.value(palette::MaterialProperty::MaterialPhase);
	}
	if (ownMaterial.has(palette::MaterialProperty::MaterialDensity)) {
		material.trdepth = ownMaterial.value(palette::MaterialProperty::MaterialDensity);
	}
	if (ownMaterial.has(palette::MaterialProperty::MaterialMedia)) {
		material.scattering = priv::toVec3f(color) * ownMaterial.value(palette::MaterialProperty::MaterialMedia);
	}
	material.opacity = color.a;
#if PATHTRACER_TEXTURES
	#error "TODO: add texture support"
#endif
	scene.materials.push_back(material);
}

#if PATHTRACER_TEXTURES
static int addEmissiveTexture(yocto::scene_data &scene, const palette::Palette &palette) {
	yocto::texture_data texture;
	texture.height = 1;
	texture.width = palette::PaletteMaxColors;
	for (int i = 0; i < palette.colorCount(); ++i) {
		const color::RGBA color = palette.emitColor(i);
		texture.pixelsb.push_back({color.r, color.g, color.b, color.a});
	}
	for (int i = palette.colorCount(); i < texture.width; ++i) {
		texture.pixelsb.push_back({});
	}
	scene.textures.push_back(texture);
	return scene.textures.size() - 1;
}

static int addPaletteTexture(yocto::scene_data &scene, const palette::Palette &palette) {
	yocto::texture_data texture;
	texture.height = 1;
	texture.width = palette::PaletteMaxColors;
	for (int i = 0; i < palette.colorCount(); ++i) {
		const color::RGBA color = palette.color(i);
		texture.pixelsb.push_back({color.r, color.g, color.b, color.a});
	}
	for (int i = palette.colorCount(); i < texture.width; ++i) {
		texture.pixelsb.push_back({});
	}
	scene.textures.push_back(texture);
	return scene.textures.size() - 1;
}
#endif

bool PathTracer::createScene(const scenegraph::SceneGraph &sceneGraph, const video::Camera *camera) {
	_state->scene = {};
	_state->lights = {};

	voxel::SurfaceExtractionType type = voxel::SurfaceExtractionType::Cubic;
	if (const core::VarPtr meshModeVar = core::getVar(cfg::VoxelMeshMode)) {
		type = (voxel::SurfaceExtractionType)meshModeVar->intVal();
		if (type >= voxel::SurfaceExtractionType::GreedyTexture) {
			type = voxel::SurfaceExtractionType::Cubic;
		}
	}
	const core::VarPtr mergeQuadsVar = core::getVar(cfg::VoxelMergeQuads);
	const bool mergeQuads = _state->studioEdges ? false : (mergeQuadsVar ? mergeQuadsVar->boolVal() : true);
	const bool reuseVertices = !_state->studioEdges;
	const bool optimizeMesh = !_state->studioEdges;
	voxel::ChunkMesh mesh(65536, 65536, true);
	for (const auto &e : sceneGraph.nodes()) {
		const scenegraph::SceneGraphNode &node = e->value;
		if (!node.isAnyModelNode()) {
			continue;
		}
		if (!node.visible()) {
			continue;
		}
		const voxel::RawVolume *v = sceneGraph.resolveVolume(node);
		if (v == nullptr) {
			continue;
		}

		const voxel::Region &region = v->region();

		const palette::Palette &palette = sceneGraph.resolvePalette(node);
		voxel::SurfaceExtractionContext ctx =
			voxel::createContext(type, v, region, palette, mesh, region.getLowerCorner(), mergeQuads, reuseVertices,
								 false, optimizeMesh);

		voxel::extractSurface(ctx);

		if (!addNode(sceneGraph, node, mesh.mesh[0], true)) {
			return false;
		}
		if (!addNode(sceneGraph, node, mesh.mesh[1], false)) {
			return false;
		}

#if PATHTRACER_TEXTURES
		const int paletteTextureIdx = addPaletteTexture(_state->scene, palette);
		const int emissiveTextureIdx = addEmissiveTexture(_state->scene, palette);
#endif
		for (int i = 0; i < palette.colorCount(); ++i) {
			setupMaterial(_state->scene, palette, i);
		}
	}

	if (_state->groundPlane) {
		addGroundPlane(sceneGraph);
	}

	if (camera) {
		addCamera("viewport", *camera);
	}

	for (auto iter = sceneGraph.begin(scenegraph::SceneGraphNodeType::Camera); iter != sceneGraph.end(); ++iter) {
		const scenegraph::SceneGraphNode &node = *iter;
		addCamera(scenegraph::toCameraNode(node));
	}

	// Only invent a framed fallback when nothing else is available.
	if (_state->scene.cameras.empty()) {
		yocto::add_camera(_state->scene);
	}

	const scenegraph::SceneGraphNode &root = sceneGraph.root();
	const core::String &sunIntensity = root.property(scenegraph::PropSunIntensity);
	if (!sunIntensity.empty()) {
		_state->sunIntensity = core::string::toFloat(sunIntensity);
		_state->sunArea = root.propertyf(scenegraph::PropSunArea);
		_state->sunElevation = glm::radians(root.propertyf(scenegraph::PropSunElevation));
		_state->sunAzimuth = glm::radians(root.propertyf(scenegraph::PropSunAzimuth));
		_state->sunDisk = root.property(scenegraph::PropSunDisk) == "true";
	}

	_state->scene.texture_names.emplace_back("sky");
	yocto::texture_data &texture = _state->scene.textures.emplace_back();
	float envAzimuth = _state->sunAzimuth;
	yocto::vec3f envEmission = {1, 1, 1};
	bool usedHdri = false;
	if (_state->hdriEnvironment && !_state->hdriPath.empty()) {
		if (priv::loadHdriTexture(_state->hdriPath, texture)) {
			usedHdri = true;
			envAzimuth = _state->hdriAzimuth;
			const float intensity = _state->hdriIntensity;
			envEmission = {intensity, intensity, intensity};
		} else {
			Log::warn("HDRI disabled after failed load");
			_state->hdriEnvironment = false;
		}
	}
	if (!usedHdri) {
		if (_state->skyEnvironment) {
			texture = yocto::image_to_texture(yocto::make_sunsky(1024, 512, _state->sunElevation, 3, _state->sunDisk,
																 _state->sunIntensity, _state->sunArea));
		} else {
			// Neutral studio wrap: slightly brighter above, still the viewport gray.
			const int envW = 256;
			const int envH = 128;
			yocto::image_data env = yocto::make_image(envW, envH, true);
			const yocto::vec3f base = _state->environmentColor;
			for (int y = 0; y < envH; ++y) {
				const float v = (float)y / (float)(envH - 1);
				const float weight = 1.12f - 0.40f * v;
				const yocto::vec3f c = base * weight;
				for (int x = 0; x < envW; ++x) {
					env[{x, y}] = {c.x, c.y, c.z, 1.0f};
				}
			}
			texture = yocto::image_to_texture(env);
		}
	}
	_state->scene.environment_names.emplace_back("sky");
	yocto::environment_data &environment = _state->scene.environments.emplace_back();
	environment.emission = envEmission;
	environment.emission_tex = (int)_state->scene.textures.size() - 1;
	environment.frame = yocto::rotation_frame({0, 1, 0}, envAzimuth);

	if (_state->params.camera < 0 || _state->params.camera >= (int)_state->scene.cameras.size()) {
		_state->params.camera = 0;
	}

	return true;
}

bool PathTracer::start(const scenegraph::SceneGraph &sceneGraph, const video::Camera *camera) {
	Log::debug("Create scene");
	applyAppearanceFromScene(sceneGraph);
	createScene(sceneGraph, camera);
	_state->bvh = yocto::make_trace_bvh(_state->scene, _state->params);
	_state->lights = yocto::make_trace_lights(_state->scene, _state->params);
	_state->state = yocto::make_trace_state(_state->scene, _state->params);
	yocto::trace_start(_state->context, _state->state, _state->scene, _state->bvh, _state->lights, _state->params);
	_state->started = true;
	Log::debug("Started pathtracer");
	return true;
}

bool PathTracer::restart(const scenegraph::SceneGraph &sceneGraph, const video::Camera *camera) {
	if (!started()) {
		return false;
	}
	Log::debug("Restart pathtracer");
	stop();
	return start(sceneGraph, camera);
}

bool PathTracer::stop() {
	yocto::trace_cancel(_state->context);
	_state->started = false;
	return true;
}

bool PathTracer::started() const {
	return _state->started;
}

bool PathTracer::update(int *currentSample) {
	if (!_state->started) {
		if (currentSample) {
			*currentSample = 0;
		}
		return true;
	}
	if (yocto::trace_done(_state->context)) {
		if (_state->state.samples >= _state->params.samples) {
			_state->started = false;
			return true;
		}
		if (currentSample) {
			*currentSample = _state->state.samples;
		}
		Log::debug("PathTracer sample: %i", _state->state.samples);
		yocto::trace_start(_state->context, _state->state, _state->scene, _state->bvh, _state->lights, _state->params);
	}
	return false;
}

image::ImagePtr PathTracer::image() {
	yocto::image_data hdr = yocto::get_image(_state->state);
	yocto::image_data image = yocto::tonemap_image(hdr, _state->exposure, _state->filmic);

	priv::YoctoImageReadStream stream(image);
	image::ImagePtr i = image::createEmptyImage("pathtracer");
	if (!i->loadRGBA(stream, image.width, image.height)) {
		return {};
	}
	return i;
}

} // namespace voxelpathtracer
