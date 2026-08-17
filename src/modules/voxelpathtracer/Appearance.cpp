/**
 * @file
 */

#include "Appearance.h"
#include "PathTracerState.h"
#include "app/App.h"
#include "core/StringUtil.h"
#include "io/File.h"
#include "io/Filesystem.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "scenegraph/SceneGraphNodeProperties.h"
#include "core/GLM.h"

namespace voxelpathtracer {

core::String resolveHdriPath(const core::String &path) {
	if (path.empty()) {
		return "";
	}
	const io::FilesystemPtr &filesystem = io::filesystem();
	io::FilePtr file = filesystem->open(path);
	if (file && file->exists()) {
		return file->name();
	}
	const core::String &basename = core::string::extractFilenameWithExtension(path);
	if (basename.empty() || basename == path) {
		return "";
	}
	file = filesystem->open(basename);
	if (file && file->exists()) {
		return file->name();
	}
	return "";
}

void applyAppearanceFromScene(PathTracerState &state, const scenegraph::SceneGraph &sceneGraph) {
	state.hdriEnvironment = false;
	state.hdriPath = "";
	state.hdriIntensity = 1.0f;
	state.hdriAzimuth = 0.0f;
	state.groundPlane = false;
	state.studioEdges = false;
	state.params.envhidden = true;

	const scenegraph::SceneGraphNode &root = sceneGraph.root();
	const core::String &hdri = root.property(scenegraph::PropHdri);
	if (!hdri.empty()) {
		state.hdriEnvironment = hdri == "true";
	}
	const core::String &hdriPath = root.property(scenegraph::PropHdriPath);
	if (!hdriPath.empty()) {
		state.hdriPath = hdriPath;
	}
	const core::String &hdriIntensity = root.property(scenegraph::PropHdriIntensity);
	if (!hdriIntensity.empty()) {
		state.hdriIntensity = core::string::toFloat(hdriIntensity);
	}
	const core::String &hdriAzimuth = root.property(scenegraph::PropHdriAzimuth);
	if (!hdriAzimuth.empty()) {
		state.hdriAzimuth = glm::radians(core::string::toFloat(hdriAzimuth));
	}
	const core::String &groundPlane = root.property(scenegraph::PropGroundPlane);
	if (!groundPlane.empty()) {
		state.groundPlane = groundPlane == "true";
	}
	const core::String &studioEdges = root.property(scenegraph::PropStudioEdges);
	if (!studioEdges.empty()) {
		state.studioEdges = studioEdges == "true";
	}
	const core::String &envHidden = root.property(scenegraph::PropEnvHidden);
	if (!envHidden.empty()) {
		state.params.envhidden = envHidden == "true";
	}
	const core::String &exposure = root.property(scenegraph::PropRenderExposure);
	if (!exposure.empty()) {
		state.exposure = core::string::toFloat(exposure);
	}
	const core::String &filmic = root.property(scenegraph::PropRenderFilmic);
	if (!filmic.empty()) {
		state.filmic = filmic == "true";
	}
}

static bool setFloatPropertyIfChanged(scenegraph::SceneGraphNode &root, const char *key, float value) {
	const core::String &cur = root.property(key);
	if (!cur.empty() && glm::abs(cur.toFloat() - value) <= 0.001f) {
		return false;
	}
	return root.setProperty(key, core::string::toString(value));
}

bool writeAppearanceToScene(const PathTracerState &state, const scenegraph::SceneGraph &sceneGraph) {
	scenegraph::SceneGraphNode &root = sceneGraph.node(sceneGraph.root().id());
	bool changed = false;
	changed |= root.setProperty(scenegraph::PropHdri, state.hdriEnvironment ? "true" : "false");
	changed |= root.setProperty(scenegraph::PropHdriPath, state.hdriPath);
	changed |= setFloatPropertyIfChanged(root, scenegraph::PropHdriIntensity, state.hdriIntensity);
	changed |= setFloatPropertyIfChanged(root, scenegraph::PropHdriAzimuth, glm::degrees(state.hdriAzimuth));
	changed |= root.setProperty(scenegraph::PropGroundPlane, state.groundPlane ? "true" : "false");
	changed |= root.setProperty(scenegraph::PropStudioEdges, state.studioEdges ? "true" : "false");
	changed |= root.setProperty(scenegraph::PropEnvHidden, state.params.envhidden ? "true" : "false");
	changed |= setFloatPropertyIfChanged(root, scenegraph::PropRenderExposure, state.exposure);
	changed |= root.setProperty(scenegraph::PropRenderFilmic, state.filmic ? "true" : "false");
	return changed;
}

} // namespace voxelpathtracer
