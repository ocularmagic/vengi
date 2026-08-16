/**
 * @file
 */

#pragma once

#include "IPathTracer.h"
#include "core/GLM.h"

namespace video {
class Camera;
}

namespace image {
class Image;
typedef core::SharedPtr<Image> ImagePtr;
} // namespace image

namespace palette {
class Palette;
}

namespace voxel {
class Mesh;
} // namespace voxel

namespace scenegraph {
class SceneGraph;
class SceneGraphNode;
class SceneGraphNodeCamera;
} // namespace scenegraph

namespace voxelpathtracer {

struct PathTracerState;

class PathTracer : public IPathTracer {
private:
	PathTracerState *_state;

	void addCamera(const scenegraph::SceneGraphNodeCamera &node);
	void addCamera(const char *name, const video::Camera &cam);

	bool addNode(const scenegraph::SceneGraph &sceneGraph, const scenegraph::SceneGraphNode &node,
				 const voxel::Mesh &mesh, bool opaque);
	bool createScene(const scenegraph::SceneGraph &sceneGraph, const video::Camera *camera);
	void addGroundPlane(const scenegraph::SceneGraph &sceneGraph);

public:
	PathTracer();
	~PathTracer() override;
	PathTracerState &state() override {
		return *_state;
	}
	const PathTracerState &state() const override {
		return *_state;
	}
	void applyAppearanceFromScene(const scenegraph::SceneGraph &sceneGraph) override;
	bool writeAppearanceToScene(const scenegraph::SceneGraph &sceneGraph) const override;
	bool start(const scenegraph::SceneGraph &sceneGraph, const video::Camera *camera = nullptr) override;
	bool restart(const scenegraph::SceneGraph &sceneGraph, const video::Camera *camera = nullptr) override;
	bool stop() override;
	bool started() const override;
	bool update(int *currentSample = nullptr) override;
	image::ImagePtr image() override;
};

} // namespace voxelpathtracer
