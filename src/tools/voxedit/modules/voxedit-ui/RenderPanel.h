/**
 * @file
 */

#pragma once

#include "image/Image.h"
#include "ui/Panel.h"
#include "video/Texture.h"
#include "voxelpathtracer/IPathTracer.h"

namespace ui {
class IMGUIApp;
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

	void renderSettings(const scenegraph::SceneGraph &sceneGraph);
	void renderMenuBar(const scenegraph::SceneGraph &sceneGraph);

public:
	RenderPanel(ui::IMGUIApp *app, const SceneManagerPtr &sceneMgr)
		: Super(app, "render"), _pathTracer(voxelpathtracer::createPathTracer()), _sceneMgr(sceneMgr) {
	}
	void update(const char *id, const scenegraph::SceneGraph &sceneGraph);
	void syncFromScene(const scenegraph::SceneGraph &sceneGraph);
	void flushToScene();
	bool init();
	void shutdown();
	void startPathTracer();
	void stopPathTracer();
#ifdef IMGUI_ENABLE_TEST_ENGINE
	void registerUITests(ImGuiTestEngine *engine, const char *id) override;
#endif
};

} // namespace voxedit
