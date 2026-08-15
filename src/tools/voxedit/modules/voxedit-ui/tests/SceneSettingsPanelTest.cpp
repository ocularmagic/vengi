/**
 * @file
 */

#include "../SceneSettingsPanel.h"
#include "TestUtil.h"
#include "core/ConfigVar.h"
#include "core/Var.h"
#include "ui/IMGUIStyle.h"
#include "ui/Style.h"
#include "voxedit-util/SceneManager.h"

namespace voxedit {

void SceneSettingsPanel::registerUITests(ImGuiTestEngine *engine, const char *id) {
	IM_REGISTER_TEST(engine, testCategory(), "shading")->TestFunc = [=](ImGuiTestContext *ctx) {
		IM_CHECK(resetScene(ctx, _sceneMgr));
		IM_CHECK(newTemplateScene(ctx, "##templates/##River"));
		IM_CHECK(focusWindow(ctx, id));

		ctx->ComboClick("Shading Mode/Unlit (Pure Colors)");
		ctx->ComboClick("Shading Mode/Lit (No Shadows)");
		ctx->ComboClick("Shading Mode/Shadows");

		changeSlider(ctx, "sunangle/Azimuth", true);
		changeSlider(ctx, "sunangle/Azimuth", false);
	};

	IM_REGISTER_TEST(engine, testCategory(), "shading modes toggle")->TestFunc = [=](ImGuiTestContext *ctx) {
		IM_CHECK(resetScene(ctx, _sceneMgr));
		IM_CHECK(focusWindow(ctx, id));

		// switch to unlit
		ctx->ComboClick("Shading Mode/Unlit (Pure Colors)");
		ctx->Yield();
		IM_CHECK_EQ(_shadingMode->intVal(), (int)ShadingMode::Unlit);
		IM_CHECK_EQ(_rendershadow->boolVal(), false);

		// switch to lit
		ctx->ComboClick("Shading Mode/Lit (No Shadows)");
		ctx->Yield();
		IM_CHECK_EQ(_shadingMode->intVal(), (int)ShadingMode::Lit);
		IM_CHECK_EQ(_rendershadow->boolVal(), false);

		// switch to shadows
		ctx->ComboClick("Shading Mode/Shadows");
		ctx->Yield();
		IM_CHECK_EQ(_shadingMode->intVal(), (int)ShadingMode::Shadows);
		IM_CHECK_EQ(_rendershadow->boolVal(), true);

		// switch to studio
		ctx->ComboClick("Shading Mode/Studio (Beveled cubes)");
		ctx->Yield();
		IM_CHECK_EQ(_shadingMode->intVal(), (int)ShadingMode::Studio);
		IM_CHECK_EQ(_rendershadow->boolVal(), false);
		IM_CHECK_EQ(core::getVar(cfg::RenderStudioBevel)->boolVal(), true);
	};

	IM_REGISTER_TEST(engine, testCategory(), "studio contrast")->TestFunc = [=](ImGuiTestContext *ctx) {
		const core::VarPtr styleVar = core::getVar(cfg::UIStyle);
		IM_CHECK(styleVar);
		const int previousStyle = styleVar->intVal();
		styleVar->setVal(ImGui::StyleStudio);
		ctx->Yield(3);

		const ImGuiStyle &imStyle = ImGui::GetStyle();
		const ImVec4 &windowBg = imStyle.Colors[ImGuiCol_WindowBg];
		const ImVec4 &frameBg = imStyle.Colors[ImGuiCol_FrameBg];
		const ImVec4 &checkboxBg = imStyle.Colors[ImGuiCol_CheckboxSelectedBg];
		const float windowLum = windowBg.x + windowBg.y + windowBg.z;
		const float frameLum = frameBg.x + frameBg.y + frameBg.z;
		const float checkboxLum = checkboxBg.x + checkboxBg.y + checkboxBg.z;
		IM_CHECK_LT(frameLum, windowLum - 0.12f);
		IM_CHECK_LT(checkboxLum, windowLum - 0.12f);
		IM_CHECK_GT(imStyle.FrameBorderSize, 0.0f);
		IM_CHECK_GE(imStyle.WindowPadding.x, 18.0f);
		IM_CHECK_GE(imStyle.ItemInnerSpacing.x, 6.0f);
		IM_CHECK_LE(imStyle.FrameRounding, 5.0f);

		const glm::vec4 &grid = style::color(style::ColorGridBorder);
		const glm::vec4 &plane = style::color(style::ColorGridPlane);
		const float viewportLum = 0.91f + 0.91f + 0.92f;
		IM_CHECK_LT(grid.r + grid.g + grid.b, viewportLum - 0.6f);
		IM_CHECK_LT(plane.r + plane.g + plane.b, viewportLum - 0.6f);

		styleVar->setVal(previousStyle);
		ctx->Yield(3);
	};

	IM_REGISTER_TEST(engine, testCategory(), "sun presets")->TestFunc = [=](ImGuiTestContext *ctx) {
		IM_CHECK(resetScene(ctx, _sceneMgr));
		IM_CHECK(focusWindow(ctx, id));

		// enable shadows mode for sun presets to work
		ctx->ComboClick("Shading Mode/Shadows");
		ctx->Yield();
		IM_CHECK_EQ(_shadingMode->intVal(), (int)ShadingMode::Shadows);
		IM_CHECK_EQ(_rendershadow->boolVal(), true);

		ctx->ItemClick("sunangle/Preset: Noon");
		ctx->Yield();
		IM_CHECK_STR_EQ(_sunAngle->strVal().c_str(), "60.00 135.00 0.00");

		ctx->ItemClick("sunangle/Preset: Evening");
		ctx->Yield();
		IM_CHECK_STR_EQ(_sunAngle->strVal().c_str(), "15.00 225.00 0.00");

		ctx->ItemClick("sunangle/Preset: Morning");
		ctx->Yield();
		IM_CHECK_STR_EQ(_sunAngle->strVal().c_str(), "15.00 45.00 0.00");
	};
}

} // namespace voxedit
