/**
 * @file
 */

#pragma once

#include "core/SharedPtr.h"

namespace video {
class Camera;
}

namespace image {
class Image;
typedef core::SharedPtr<Image> ImagePtr;
} // namespace image

namespace scenegraph {
class SceneGraph;
}

namespace voxelpathtracer {

struct PathTracerState;

/**
 * @brief Backend-neutral path tracer used by the Render panel.
 *
 * The Render panel uses this contract. createPathTracer() currently returns the
 * CPU voxel-grid tracer. The Yocto mesh tracer remains available as PathTracer.
 */
class IPathTracer {
public:
	virtual ~IPathTracer() {
	}

	virtual PathTracerState &state() = 0;
	virtual const PathTracerState &state() const = 0;

	virtual void applyAppearanceFromScene(const scenegraph::SceneGraph &sceneGraph) = 0;
	virtual bool writeAppearanceToScene(const scenegraph::SceneGraph &sceneGraph) const = 0;

	virtual bool start(const scenegraph::SceneGraph &sceneGraph, const video::Camera *camera = nullptr) = 0;
	virtual bool restart(const scenegraph::SceneGraph &sceneGraph, const video::Camera *camera = nullptr) = 0;
	virtual bool stop() = 0;
	virtual bool started() const = 0;

	/**
	 * @return @c true if rendering is done, @c false otherwise
	 */
	virtual bool update(int *currentSample = nullptr) = 0;
	virtual image::ImagePtr image() = 0;
};

IPathTracer *createPathTracer();

} // namespace voxelpathtracer
