/**
 * @file
 */

#include "../RenderPanel.h"
#include "../WindowTitles.h"
#include "voxedit-util/SceneManager.h"
#include "voxelpathtracer/PathTracerState.h"

namespace voxedit {

void RenderPanel::registerUITests(ImGuiTestEngine *engine, const char *id) {
	IM_REGISTER_TEST(engine, testCategory(), "settings inputs")->TestFunc = [=](ImGuiTestContext *ctx) {
		IM_CHECK(focusWindow(ctx, TITLE_RENDER));
		voxelpathtracer::PathTracerState &state = _pathTracer->state();
		yocto::trace_params &params = state.params;

		ctx->MenuAction(ImGuiTestAction_Open, "Settings/Output");
		const int oldResolution = params.resolution;
		ctx->ItemInputValue("//$FOCUSED/Dimensions", oldResolution + 128);
		IM_CHECK(params.resolution == oldResolution + 128);
		const float oldExposure = state.exposure;
		ctx->ItemInputValue("//$FOCUSED/Exposure", oldExposure + 1.0f);
		IM_CHECK_EQ(state.exposure, oldExposure + 1.0f);
		const bool oldFilmic = state.filmic;
		ctx->ItemClick("//$FOCUSED/Filmic");
		IM_CHECK_EQ(state.filmic, !oldFilmic);

		ctx->MenuAction(ImGuiTestAction_Open, "Settings/Quality");
		const int oldSamples = params.samples;
		ctx->ItemInputValue("//$FOCUSED/Samples", oldSamples + 16);
		IM_CHECK(params.samples == oldSamples + 16);
	};

	IM_REGISTER_TEST(engine, testCategory(), "presets")->TestFunc = [=](ImGuiTestContext *ctx) {
		IM_CHECK(focusWindow(ctx, TITLE_RENDER));
		voxelpathtracer::PathTracerState &state = _pathTracer->state();
		yocto::trace_params &params = state.params;

		ctx->MenuClick("Settings/Presets/High quality");
		ctx->Yield();
		IM_CHECK_EQ(params.samples, 1024);
		IM_CHECK_EQ(params.bounces, 64);

		ctx->MenuClick("Settings/Presets/Geometry preview");
		ctx->Yield();
		IM_CHECK_EQ(params.samples, 16);

		ctx->MenuClick("Settings/Presets/Reset all");
		ctx->Yield();
		yocto::trace_params defaults;
		IM_CHECK_EQ(params.samples, defaults.samples);
		IM_CHECK_EQ(params.bounces, defaults.bounces);
		IM_CHECK_EQ(state.skyEnvironment, false);

		ctx->MenuClick("Settings/Presets/Studio");
		ctx->Yield();
		IM_CHECK_EQ(state.skyEnvironment, false);
		IM_CHECK_EQ(state.hdriEnvironment, false);
		IM_CHECK_EQ(state.environmentColor.x, 0.91f);
	};

	IM_REGISTER_TEST(engine, testCategory(), "lighting appearance")->TestFunc = [=](ImGuiTestContext *ctx) {
		IM_CHECK(focusWindow(ctx, TITLE_RENDER));
		voxelpathtracer::PathTracerState &state = _pathTracer->state();

		ctx->MenuAction(ImGuiTestAction_Open, "Settings/Lighting");
		ctx->ItemClick("//$FOCUSED/HDRI image");
		IM_CHECK_EQ(state.hdriEnvironment, true);

		ctx->MenuAction(ImGuiTestAction_Open, "Settings/Advanced");
		ctx->ItemClick("//$FOCUSED/Ground plane");
		IM_CHECK_EQ(state.groundPlane, true);
		ctx->ItemClick("//$FOCUSED/Voxel edges");
		IM_CHECK_EQ(state.studioEdges, true);
	};

	IM_REGISTER_TEST(engine, testCategory(), "view menu and camera reset")->TestFunc = [=](ImGuiTestContext *ctx) {
		IM_CHECK(focusWindow(ctx, TITLE_RENDER));
		ctx->MenuAction(ImGuiTestAction_Open, "View");
		ctx->ItemClick("//$FOCUSED/Reset camera");
		ctx->Yield();
	};
}

} // namespace voxedit
