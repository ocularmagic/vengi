/**
 * @file
 */

#include "app/tests/AbstractTest.h"
#include "imgui.h"
#include "ui/IMGUIStyle.h"

namespace voxedit {

class StudioStyleTest : public app::AbstractTest {};

TEST_F(StudioStyleTest, testStudioWidgetContrast) {
	ImGuiContext *ctx = ImGui::CreateContext();
	ASSERT_NE(ctx, nullptr);
	ImGui::StyleColorsStudio();

	const ImGuiStyle &style = ImGui::GetStyle();
	const ImVec4 &windowBg = style.Colors[ImGuiCol_WindowBg];
	const ImVec4 &frameBg = style.Colors[ImGuiCol_FrameBg];
	const ImVec4 &checkboxBg = style.Colors[ImGuiCol_CheckboxSelectedBg];
	const float windowLum = windowBg.x + windowBg.y + windowBg.z;
	const float frameLum = frameBg.x + frameBg.y + frameBg.z;
	const float checkboxLum = checkboxBg.x + checkboxBg.y + checkboxBg.z;
	EXPECT_LT(frameLum, windowLum - 0.12f);
	EXPECT_LT(checkboxLum, windowLum - 0.12f);
	EXPECT_GT(style.FrameBorderSize, 0.0f);
	EXPECT_GE(style.WindowPadding.x, 18.0f);
	EXPECT_GE(style.ItemInnerSpacing.x, 6.0f);
	EXPECT_GE(style.FrameRounding, 3.0f);
	EXPECT_LE(style.FrameRounding, 5.0f);

	ImGui::DestroyContext(ctx);
}

} // namespace voxedit
