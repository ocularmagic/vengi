/**
 * @file
 */

#pragma once

#include "core/Var.h"
#include "image/Image.h"
#include "ui/Panel.h"
#include "video/Camera.h"
#include "video/Texture.h"
#include "voxelpathtracer/IPathTracer.h"
#include "voxelrender/RenderUtil.h"

namespace ui {
class IMGUIApp;
}

namespace command {
struct CommandExecutionListener;
}

namespace voxedit {

class SceneManager;
typedef core::SharedPtr<SceneManager> SceneManagerPtr;

class RenderPanel : public ui::Panel {
private:
	using Super = ui::Panel;
	voxelpathtracer::IPathTracer *_pathTracer;
	video::TexturePtr _texture;
	image::ImagePtr _image;
	SceneManagerPtr _sceneMgr;
	int _currentSample = 0;

	video::Camera _camera;
	voxelrender::SceneCameraMode _camMode = voxelrender::SceneCameraMode::Free;
	bool _cameraInitialized = false;
	bool _cameraManipulated = false;
	core::VarPtr _viewDistance;
	core::VarPtr _gizmoAllowAxisFlip;

	void renderSettings(const scenegraph::SceneGraph &sceneGraph);
	void renderMenuBar(const scenegraph::SceneGraph &sceneGraph);
	void renderCameraManipulator(video::Camera &camera, float headerSize);

public:
	RenderPanel(ui::IMGUIApp *app, const SceneManagerPtr &sceneMgr);
	void update(const char *id, const scenegraph::SceneGraph &sceneGraph);
	void syncFromScene(const scenegraph::SceneGraph &sceneGraph);
	void flushToScene();
	bool init();
	void shutdown();
	void startPathTracer();
	void stopPathTracer();
	void resetCamera();
	void setCamMode(voxelrender::SceneCameraMode mode);
	bool setHdri(const core::String &filename);

	const video::Camera &camera() const { return _camera; }
	video::Camera &camera() { return _camera; }

#ifdef IMGUI_ENABLE_TEST_ENGINE
	void registerUITests(ImGuiTestEngine *engine, const char *id) override;
#endif
};

} // namespace voxedit
