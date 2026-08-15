/**
 * @file
 */

#include "StudioHud.h"
#include "BrushPanelCommon.h"
#include "app/I18N.h"
#include "command/CommandHandler.h"
#include "color/RGBA.h"
#include "memento/MementoHandler.h"
#include "palette/Palette.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "ui/IMGUIEx.h"
#include "ui/IconsLucide.h"
#include "ui/ScopedStyle.h"
#include "ui/Style.h"
#include "ui/Toolbar.h"
#include "voxedit-util/Config.h"
#include "voxedit-util/SceneManager.h"
#include "voxedit-util/modifier/Modifier.h"
#include "voxedit-util/modifier/ModifierType.h"
#include "voxedit-util/modifier/brush/BrushType.h"
#include "voxel/Region.h"
#include "voxel/Voxel.h"

namespace voxedit {
namespace studiohud {

namespace {

static const ImGuiWindowFlags OverlayFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
											 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
											 ImGuiWindowFlags_NoFocusOnAppearing;

static bool beginOverlay(const char *id, const ImVec2 &pos, const ImVec2 &pivot) {
	ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
	ImGui::SetNextWindowBgAlpha(0.94f);
	ui::ScopedStyle style;
	style.setWindowRounding(12.0f);
	style.setWindowPadding(ImVec2(10.0f, 8.0f));
	style.setWindowBorderSize(1.0f);
	return ImGui::Begin(id, nullptr, OverlayFlags);
}

static bool overlayHovered() {
	return ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
}

static const scenegraph::SceneGraphNode *activeModel(const SceneManagerPtr &sceneMgr) {
	const int nodeId = sceneMgr->sceneGraph().activeNode();
	return sceneMgr->sceneGraphModelNode(nodeId);
}

static bool renderInfoCard(const SceneManagerPtr &sceneMgr, const ImVec2 &windowPos, float headerSize) {
	const ImVec2 pos(windowPos.x + 12.0f, windowPos.y + headerSize + 12.0f);
	if (!beginOverlay("##studiohud-card", pos, ImVec2(0.0f, 0.0f))) {
		ImGui::End();
		return false;
	}
	const bool hovered = overlayHovered();
	const scenegraph::SceneGraphNode *node = activeModel(sceneMgr);
	if (node != nullptr && node->region().isValid()) {
		const voxel::Region &region = node->region();
		const glm::ivec3 origin = region.getLowerCorner();
		const glm::ivec3 size = region.getDimensionsInVoxels();
		ImGui::TextDisabled("%s", _("ORIGIN"));
		ImGui::SameLine();
		ImGui::Text("%i  %i  %i", origin.x, origin.y, origin.z);
		ImGui::TextDisabled("%s", _("SIZE"));
		ImGui::SameLine();
		ImGui::Text("%i  %i  %i", size.x, size.y, size.z);
	} else {
		ImGui::TextDisabled("%s", _("No model"));
	}
	ImGui::End();
	return hovered;
}

static bool renderPaletteStrip(const SceneManagerPtr &sceneMgr, const ImVec2 &windowPos, const ImVec2 &contentSize,
							   float headerSize) {
	if (contentSize.y < 80.0f) {
		return false;
	}
	const ImVec2 pos(windowPos.x + 12.0f, windowPos.y + headerSize + 78.0f);
	ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.94f);
	ImGui::SetNextWindowSizeConstraints(ImVec2(44.0f, 80.0f), ImVec2(56.0f, contentSize.y * 0.55f));
	ui::ScopedStyle style;
	style.setWindowRounding(12.0f);
	style.setWindowPadding(ImVec2(8.0f, 8.0f));
	style.setWindowBorderSize(1.0f);
	if (!ImGui::Begin("##studiohud-palette", nullptr, OverlayFlags | ImGuiWindowFlags_NoScrollbar)) {
		ImGui::End();
		return false;
	}
	bool hovered = overlayHovered();
	const scenegraph::SceneGraphNode *node = activeModel(sceneMgr);
	if (node == nullptr) {
		ImGui::End();
		return hovered;
	}
	const palette::Palette &palette = node->palette();
	Modifier &modifier = sceneMgr->modifier();
	const uint8_t selected = modifier.cursorVoxel().getColor();
	const float swatch = ImGui::GetFrameHeight();
	int shown = 0;
	for (int i = 0; i < palette.colorCount() && shown < 16; ++i) {
		const uint8_t idx = palette.view().uiIndex((uint8_t)i);
		const color::RGBA rgba = palette.color(idx);
		if (rgba.a == 0) {
			continue;
		}
		ImGui::PushID(i);
		const ImVec4 col((float)rgba.r / 255.0f, (float)rgba.g / 255.0f, (float)rgba.b / 255.0f,
						 (float)rgba.a / 255.0f);
		if (ImGui::ColorButton("##swatch", col, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
							   ImVec2(swatch, swatch))) {
			modifier.setCursorVoxel(voxel::createVoxel(palette, idx));
		}
		if (idx == selected) {
			const ImVec2 p0 = ImGui::GetItemRectMin();
			const ImVec2 p1 = ImGui::GetItemRectMax();
			ImGui::GetWindowDrawList()->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Text), 4.0f, 2.0f);
		}
		ImGui::PopID();
		++shown;
	}

	int gridSize = core::getVar(cfg::VoxEditGridsize)->intVal();
	ImGui::SetNextItemWidth(swatch);
	if (ImGui::VSliderInt("##studiosize", ImVec2(swatch, 80.0f), &gridSize, 1, 16, "%d")) {
		core::getVar(cfg::VoxEditGridsize)->setVal(gridSize);
	}
	ImGui::End();
	return hovered;
}

static bool renderTools(const SceneManagerPtr &sceneMgr, const ImVec2 &windowPos, const ImVec2 &contentSize,
						float headerSize, command::CommandExecutionListener *listener) {
	const ImVec2 pos(windowPos.x + 12.0f, windowPos.y + headerSize + contentSize.y - 12.0f);
	if (!beginOverlay("##studiohud-tools", pos, ImVec2(0.0f, 1.0f))) {
		ImGui::End();
		return false;
	}
	const bool hovered = overlayHovered();
	const memento::MementoHandler &mementoHandler = sceneMgr->mementoHandler();
	ui::Toolbar toolbar("studiotools", listener);
	toolbar.button(ICON_LC_UNDO, "undo", !mementoHandler.canUndo());
	toolbar.button(ICON_LC_REDO, "redo", !mementoHandler.canRedo());
	toolbar.button(ICON_LC_GRID_3X3, "toggle ve_showgrid");

	Modifier &modifier = sceneMgr->modifier();
	const BrushType brushType = modifier.brushType();
	const BrushType tools[] = {BrushType::Shape, BrushType::Paint, BrushType::Select, BrushType::Sculpt};
	for (const BrushType type : tools) {
		const int idx = (int)type;
		core::String cmd = core::String::format("brush%s", BrushTypeStr[idx]).toLower();
		const bool current = brushType == type;
		ui::ScopedStyle styleButton;
		if (current) {
			styleButton.setButtonColor(style::color(style::ColorActiveBrush));
		}
		auto func = [listener, cmd]() { command::executeCommands(cmd, listener); };
		toolbar.button(BrushTypeIcons[idx], BrushTypeStr[idx], func, !current);
	}
	if ((modifier.checkModifierType() & ModifierType::Erase) != ModifierType::None) {
		toolbar.button(ICON_LC_ERASER, "actionerase", !modifier.isMode(ModifierType::Erase));
	}
	toolbar.end();
	ImGui::End();
	return hovered;
}

} // namespace

bool render(const SceneManagerPtr &sceneMgr, bool sceneMode, const ImVec2 &windowPos, const ImVec2 &contentSize,
			float headerSize, command::CommandExecutionListener *listener) {
	bool hovered = renderInfoCard(sceneMgr, windowPos, headerSize);
	if (!sceneMode) {
		hovered |= renderPaletteStrip(sceneMgr, windowPos, contentSize, headerSize);
	}
	hovered |= renderTools(sceneMgr, windowPos, contentSize, headerSize, listener);
	return hovered;
}

} // namespace studiohud
} // namespace voxedit
