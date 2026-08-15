/**
 * @file
 */

#pragma once

#include "core/SharedPtr.h"
#include <imgui.h>

namespace command {
struct CommandExecutionListener;
}

namespace voxedit {

class SceneManager;
typedef core::SharedPtr<SceneManager> SceneManagerPtr;

namespace studiohud {

/**
 * @brief Floating Studio chrome over the edit viewport.
 * @return true if the mouse is over any Studio overlay
 */
bool render(const SceneManagerPtr &sceneMgr, bool sceneMode, const ImVec2 &windowPos, const ImVec2 &contentSize,
			float headerSize, command::CommandExecutionListener *listener);

} // namespace studiohud
} // namespace voxedit
