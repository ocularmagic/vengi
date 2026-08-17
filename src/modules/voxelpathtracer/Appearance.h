/**
 * @file
 */

#pragma once

#include "core/String.h"

namespace scenegraph {
class SceneGraph;
}

namespace voxelpathtracer {

struct PathTracerState;

/**
 * Resolve an HDRI reference through the application filesystem. If a scene
 * contains an unavailable absolute path, also try its basename so browser
 * uploads and portable scene assets can satisfy the reference.
 */
core::String resolveHdriPath(const core::String &path);

void applyAppearanceFromScene(PathTracerState &state, const scenegraph::SceneGraph &sceneGraph);
bool writeAppearanceToScene(const PathTracerState &state, const scenegraph::SceneGraph &sceneGraph);

} // namespace voxelpathtracer
