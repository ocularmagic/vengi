/**
 * @file
 */

#include "SceneSettingsPanel.h"
#include "ui/IMGUIEx.h"
#include "ui/IconsLucide.h"
#include "ui/ScopedID.h"
#include "ui/ScopedPanel.h"
#include "ui/ScopedStyle.h"
#include "core/ConfigVar.h"
#include "core/Var.h"
#include "voxedit-util/Config.h"
#include "voxedit-util/SceneManager.h"

namespace voxedit {

SceneSettingsPanel::SceneSettingsPanel(ui::IMGUIApp *app, const SceneManagerPtr &sceneMgr)
	: Super(app, "scenesettings"), _sceneMgr(sceneMgr) {
}

void applyShadingMode(ShadingMode shadingMode) {
	core::VarPtr rendershadow = core::getVar(cfg::VoxEditRendershadow);
	core::VarPtr ambientColor = core::getVar(cfg::VoxEditAmbientColor);
	core::VarPtr diffuseColor = core::getVar(cfg::VoxEditDiffuseColor);
	core::VarPtr sunAngle = core::getVar(cfg::VoxEditSunAngle);
	core::VarPtr studioBevel = core::getVar(cfg::RenderStudioBevel);
	core::VarPtr shadingVar = core::getVar(cfg::VoxEditShadingMode);
	if (shadingVar) {
		shadingVar->setVal((int)shadingMode);
	}

	switch (shadingMode) {
	case ShadingMode::Unlit:
		if (rendershadow) {
			rendershadow->setVal(false);
		}
		if (ambientColor) {
			ambientColor->setVal("1.0 1.0 1.0");
		}
		if (diffuseColor) {
			diffuseColor->setVal("0.0 0.0 0.0");
		}
		if (studioBevel) {
			studioBevel->setVal(false);
		}
		break;
	case ShadingMode::Lit:
		if (rendershadow) {
			rendershadow->setVal(false);
		}
		if (ambientColor) {
			ambientColor->setVal("0.3 0.3 0.3");
		}
		if (diffuseColor) {
			diffuseColor->setVal("0.7 0.7 0.7");
		}
		if (studioBevel) {
			studioBevel->setVal(false);
		}
		break;
	case ShadingMode::Shadows:
		if (rendershadow) {
			rendershadow->setVal(true);
		}
		if (ambientColor) {
			ambientColor->setVal("0.3 0.3 0.3");
		}
		if (diffuseColor) {
			diffuseColor->setVal("0.7 0.7 0.7");
		}
		if (sunAngle) {
			sunAngle->setVal("45.0 135.0 0.0");
		}
		if (studioBevel) {
			studioBevel->setVal(false);
		}
		break;
	case ShadingMode::Studio:
		if (rendershadow) {
			rendershadow->setVal(false);
		}
		if (ambientColor) {
			ambientColor->setVal("0.72 0.72 0.74");
		}
		if (diffuseColor) {
			diffuseColor->setVal("0.35 0.35 0.32");
		}
		if (sunAngle) {
			sunAngle->setVal("55.0 135.0 0.0");
		}
		if (studioBevel) {
			studioBevel->setVal(true);
		}
		break;
	}
}

void SceneSettingsPanel::init() {
	_rendershadow = core::getVar(cfg::VoxEditRendershadow);
	_ambientColor = core::getVar(cfg::VoxEditAmbientColor);
	_diffuseColor = core::getVar(cfg::VoxEditDiffuseColor);
	_sunAngle = core::getVar(cfg::VoxEditSunAngle);
	_shadingMode = core::getVar(cfg::VoxEditShadingMode);
}

void SceneSettingsPanel::sceneColors(ShadingMode shadingMode) {
	const bool lightingEnabled = (shadingMode != ShadingMode::Unlit);

	ui::ScopedStyle style;
	if (!lightingEnabled) {
		style.setAlpha(ImGui::GetStyle().Alpha * 0.5f);
	}

	ImGui::ColorEdit3Var(cfg::VoxEditAmbientColor);
	if (lightingEnabled) {
		ImGui::SetItemTooltipUnformatted(_("Base lighting that affects all surfaces equally"));
	} else {
		ImGui::SetItemTooltipUnformatted(_("Ambient color is disabled in Unlit mode"));
	}

	ImGui::ColorEdit3Var(cfg::VoxEditDiffuseColor);
	if (lightingEnabled) {
		ImGui::SetItemTooltipUnformatted(_("Directional lighting that varies based on surface angle"));
	} else {
		ImGui::SetItemTooltipUnformatted(_("Diffuse color is disabled in Unlit mode"));
	}
}

void SceneSettingsPanel::sceneShadowAndSun(ShadingMode shadingMode) {
	const bool shadowsEnabled = (shadingMode == ShadingMode::Shadows);
	ui::ScopedStyle style;
	// Sun angle widget - only enabled when shadows are on
	if (!shadowsEnabled) {
		style.setAlpha(ImGui::GetStyle().Alpha * 0.5f);
	}

	ImGui::TextUnformatted(_("Sun angle"));
	if (shadowsEnabled) {
		ImGui::SetItemTooltipUnformatted(_("Controls the direction of the sun for shadow casting"));
	} else {
		ImGui::SetItemTooltipUnformatted(_("Sun angle is only used in Shadows mode"));
	}

	glm::vec3 sunAngle;
	_sunAngle->vec3Val(&sunAngle[0]);

	bool sunChanged = false;
	ui::ScopedID id("sunangle");
	if (ImGui::SliderFloat(_("Elevation"), &sunAngle.x, -90.0f, 90.0f, "%.1f°")) {
		sunChanged = shadowsEnabled;
	}
	ImGui::SetItemTooltipUnformatted(_("Sun elevation angle (pitch): -90 (below) to +90 (above)"));

	if (ImGui::SliderFloat(_("Azimuth"), &sunAngle.y, 0.0f, 360.0f, "%.1f°")) {
		sunChanged = shadowsEnabled;
	}
	ImGui::SetItemTooltipUnformatted(_("Sun azimuth angle (yaw): 0 (North) to 360"));

	ImGui::BeginDisabled(!shadowsEnabled);
	if (ImGui::Button(_("Preset: Noon"))) {
		sunAngle = glm::vec3(60.0f, 135.0f, 0.0f);
		sunChanged = true;
	}
	ImGui::SameLine();
	if (ImGui::Button(_("Preset: Evening"))) {
		sunAngle = glm::vec3(15.0f, 225.0f, 0.0f);
		sunChanged = true;
	}
	ImGui::SameLine();
	if (ImGui::Button(_("Preset: Morning"))) {
		sunAngle = glm::vec3(15.0f, 45.0f, 0.0f);
		sunChanged = true;
	}
	ImGui::EndDisabled();

	if (sunChanged) {
		const core::String val = core::String::format("%.2f %.2f %.2f", sunAngle.x, sunAngle.y, sunAngle.z);
		_sunAngle->setVal(val);
	}
}

void SceneSettingsPanel::update(const char *id, command::CommandExecutionListener &listener) {
	static ui::ScopedPanel panel(cfg::VoxEditShowSceneSettings);
	if (!panel.isOpen()) {
		return;
	}
	const core::String &title = makeTitle(ICON_LC_SPOTLIGHT, _("Scene settings"), id);
	if (ui::ScopedPanel::Scope scope = panel.begin(title.c_str(), ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::TextUnformatted(_("Scene settings"));
		ImGui::Separator();

		const char *shadingModeItems[] = {_("Unlit (Pure Colors)"), _("Lit (No Shadows)"), _("Shadows"),
										  _("Studio (Beveled cubes)")};
		int currentShadingMode = _shadingMode->intVal();
		ShadingMode shadingMode = (ShadingMode)currentShadingMode;

		if (ImGui::Combo(_("Shading Mode"), &currentShadingMode, shadingModeItems, lengthof(shadingModeItems))) {
			shadingMode = (ShadingMode)currentShadingMode;
			applyShadingMode(shadingMode);
		}

		sceneColors(shadingMode);
		sceneShadowAndSun(shadingMode);
	}
}

} // namespace voxedit
