/**
 * @file
 */

#include "RenderPanel.h"
#include "core/SharedPtr.h"
#include "io/FileStream.h"
#include "io/FormatDescription.h"
#include "scenegraph/SceneGraph.h"
#include "ui/IMGUIApp.h"
#include "ui/IMGUIEx.h"
#include "ui/IconsLucide.h"
#include "ui/ScopedPanel.h"
#include "video/Texture.h"
#include "voxedit-util/Config.h"
#include "voxedit-util/SceneManager.h"
#include "voxelpathtracer/IPathTracer.h"
#include "voxelpathtracer/PathTracerState.h"

namespace voxedit {

bool RenderPanel::init() {
	_texture = video::createEmptyTexture("pathtracer");
	return true;
}

void RenderPanel::renderSettings(const scenegraph::SceneGraph &sceneGraph) {
	voxelpathtracer::PathTracerState &state = _pathTracer->state();
	yocto::trace_params &params = state.params;
	int changed = 0;
	bool postprocessChanged = false;

	const float itemWidth = ImGui::GetFontSize() * 10.0f;

	if (ImGui::BeginIconMenu(ICON_LC_SPARKLES, _("Presets"))) {
		if (ImGui::IconMenuItem(ICON_LC_ROTATE_CCW, _("Reset all"))) {
			state.resetAppearance();
			++changed;
		}
		ImGui::TooltipTextUnformatted(_("Restore default path tracer settings"));
		if (ImGui::IconMenuItem(ICON_LC_BOXES, _("Studio"))) {
			state.resetAppearance();
			++changed;
		}
		ImGui::TooltipTextUnformatted(_("Match the Studio edit viewport: gray wrap lighting, no blue sky"));
		if (ImGui::IconMenuItem(ICON_LC_SPARKLES, _("High quality"))) {
			params = yocto::trace_params();
			params.sampler = yocto::trace_sampler_type::path;
			params.samples = 1024;
			params.bounces = 64;
			++changed;
		}
		ImGui::TooltipTextUnformatted(_("Path tracing with high sample and bounce counts"));
		if (ImGui::IconMenuItem(ICON_LC_BOX, _("Geometry preview"))) {
			params = yocto::trace_params();
			params.sampler = yocto::trace_sampler_type::eyelight;
			params.samples = 16;
			++changed;
		}
		ImGui::TooltipTextUnformatted(_("Fast eyelight preview for checking geometry"));
		ImGui::EndMenu();
	}

	if (ImGui::BeginIconMenu(ICON_LC_GAUGE, _("Quality"))) {
		ImGui::PushItemWidth(itemWidth);
		changed += ImGui::ComboItems(_("Tracer"), (int *)&params.sampler, yocto::trace_sampler_names);
		changed += ImGui::InputInt(_("Samples"), &params.samples, 16, 4096);
		ImGui::TooltipTextUnformatted(_("Per-pixel samples. Higher values reduce noise but take longer."));
		changed += ImGui::SliderInt(_("Bounces"), &params.bounces, 1, 128);
		ImGui::TooltipTextUnformatted(_("Maximum light bounces. Increase for glass and volumes."));
		changed += ImGui::SliderFloat(_("Clamp"), &params.clamp, 10, 1000);
		ImGui::TooltipTextUnformatted(_("Remove high-energy fireflies"));
		ImGui::Separator();
		changed += ImGui::SliderInt(_("Preview ratio"), &params.pratio, 1, 64);
		ImGui::TooltipTextUnformatted(_("Lower resolution ratio used while samples accumulate"));
		changed += ImGui::SliderInt(_("Batch"), &params.batch, 1, 16);
		ImGui::TooltipTextUnformatted(_("Samples computed per update step"));
		ImGui::Separator();
		changed += ImGui::Checkbox(_("Adaptive sampling"), &params.adaptive);
		ImGui::TooltipTextUnformatted(_("Stop each pixel once its mean stabilizes; only still-noisy pixels keep getting rays. Samples becomes a safety cap."));
		ImGui::BeginDisabled(!params.adaptive);
		changed += ImGui::SliderFloat(_("Adaptive error"), &params.adaptiveError, 0.001f, 0.2f);
		ImGui::TooltipTextUnformatted(_("Relative standard-error threshold for a pixel to stop sampling"));
		ImGui::EndDisabled();
		ImGui::PopItemWidth();
		ImGui::EndMenu();
	}

	if (ImGui::BeginIconMenu(ICON_LC_IMAGE, _("Output"))) {
		ImGui::PushItemWidth(itemWidth);
		changed += ImGui::InputInt(_("Dimensions"), &params.resolution);
		ImGui::TooltipTextUnformatted(_("Output image size in pixels (square)"));
		changed += ImGui::Checkbox(_("Filter"), &params.tentfilter);
		ImGui::TooltipTextUnformatted(_("Apply a linear filter to the image pixels"));
		postprocessChanged |= ImGui::Checkbox(_("Denoise"), &params.denoise);
		ImGui::TooltipTextUnformatted(
			_("Post-process on the current picture (not extra samples). Averages grain on each voxel face and keeps 90-degree edges. Toggle anytime; it does not restart the tracer."));
		ImGui::Separator();
		postprocessChanged |= ImGui::SliderFloat(_("Exposure"), &state.exposure, -5.0f, 5.0f);
		ImGui::TooltipTextUnformatted(_("Exposure compensation in stops for tonemapping."));
		postprocessChanged |= ImGui::Checkbox(_("Filmic"), &state.filmic);
		ImGui::TooltipTextUnformatted(_("Use filmic tonemapping for softer highlight rolloff."));
		ImGui::PopItemWidth();
		ImGui::EndMenu();
	}

	if (ImGui::BeginIconMenu(ICON_LC_CAMERA, _("Camera"))) {
		ImGui::PushItemWidth(itemWidth);
		if (!state.scene.camera_names.empty()) {
			changed += ImGui::ComboItems(_("Camera"), &params.camera, state.scene.camera_names);
		}
		changed += ImGui::SliderFloat(_("Aperture"), &state.aperture, 0.0f, 0.5f);
		ImGui::TooltipTextUnformatted(_("Lens aperture for depth of field. 0 means pinhole (no DOF)."));
		ImGui::PopItemWidth();
		ImGui::EndMenu();
	}

	if (ImGui::BeginIconMenu(ICON_LC_SUN, _("Lighting"))) {
		ImGui::PushItemWidth(itemWidth);
		ImGui::BeginDisabled(state.hdriEnvironment);
		changed += ImGui::Checkbox(_("Sky environment"), &state.skyEnvironment);
		ImGui::TooltipTextUnformatted(_("Use a physical sun and sky. Off matches the Studio edit viewport."));
		float envColor[3] = {state.environmentColor.x, state.environmentColor.y, state.environmentColor.z};
		if (ImGui::ColorEdit3(_("Environment"), envColor)) {
			state.environmentColor = {envColor[0], envColor[1], envColor[2]};
			++changed;
		}
		ImGui::TooltipTextUnformatted(_("Background and wrap lighting when sky is off"));
		ImGui::EndDisabled();
		changed += ImGui::Checkbox(_("HDRI image"), &state.hdriEnvironment);
		ImGui::TooltipTextUnformatted(_("Light the scene from a Radiance .hdr environment map."));
		ImGui::BeginDisabled(!state.hdriEnvironment);
		if (ImGui::InputText(_("HDRI file"), &state.hdriPath)) {
			++changed;
		}
		ImGui::SameLine();
		if (ImGui::Button(ICON_LC_FILE "##hdribrowse")) {
			static const io::FormatDescription hdriFormats[] = {
				{"Radiance rgbE", "image/vnd.radiance", {"hdr"}, {}, 0u},
				{"OpenEXR", "image/x-exr", {"exr"}, {}, 0u},
				io::FormatDescription::END};
			_app->openDialog(
				[this](const core::String &filename, const io::FormatDescription *) {
					voxelpathtracer::PathTracerState &st = _pathTracer->state();
					st.hdriEnvironment = true;
				#ifdef __EMSCRIPTEN__
					// Browser uploads live in the application home filesystem. Store
					// a portable reference instead of an Emscripten mount path.
					st.hdriPath = core::string::extractFilenameWithExtension(filename);
				#else
					st.hdriPath = filename;
				#endif
					_pathTracer->writeAppearanceToScene(_sceneMgr->sceneGraph());
					_sceneMgr->markDirty();
					if (_pathTracer->started()) {
						_pathTracer->restart(_sceneMgr->sceneGraph(), _sceneMgr->activeCamera());
					}
				},
				{}, hdriFormats);
		}
		changed += ImGui::SliderFloat(_("HDRI intensity"), &state.hdriIntensity, 0.0f, 10.0f);
		changed += ImGui::SliderAngle(_("HDRI azimuth"), &state.hdriAzimuth, 0.0f, 360.0f);
		ImGui::TooltipTextUnformatted(_("Rotate the HDRI around the vertical axis."));
		ImGui::EndDisabled();
		ImGui::BeginDisabled(!state.skyEnvironment || state.hdriEnvironment);
		changed += ImGui::SliderFloat(_("Sun intensity"), &state.sunIntensity, 0.0f, 10.0f);
		changed += ImGui::SliderFloat(_("Sun area"), &state.sunArea, 0.0f, 5.0f);
		ImGui::TooltipTextUnformatted(_("Sun disk size. 1.0 is about 43.5 degrees."));
		changed += ImGui::SliderAngle(_("Sun elevation"), &state.sunElevation, 0.0f, 90.0f);
		changed += ImGui::SliderAngle(_("Sun azimuth"), &state.sunAzimuth, 0.0f, 360.0f);
		changed += ImGui::Checkbox(_("Sun disk"), &state.sunDisk);
		ImGui::TooltipTextUnformatted(_("Show visible sun disk in the sky."));
		ImGui::EndDisabled();
		ImGui::Separator();
		changed += ImGui::Checkbox(_("Hide environment"), &params.envhidden);
		ImGui::TooltipTextUnformatted(
			_("Hide the backdrop (transparent pixels) while keeping environment lighting. On by default. Saved with the scene."));
		ImGui::PopItemWidth();
		ImGui::EndMenu();
	}

	if (ImGui::BeginIconMenu(ICON_LC_WRENCH, _("Advanced"))) {
		changed += ImGui::Checkbox(_("Ground plane"), &state.groundPlane);
		ImGui::TooltipTextUnformatted(_("Light grey floor under the lowest voxel to receive shadows."));
		changed += ImGui::Checkbox(_("Voxel edges"), &state.studioEdges);
		ImGui::TooltipTextUnformatted(_("Soft studio bevel on every exposed voxel face."));
		changed += ImGui::Checkbox(_("No caustics"), &params.nocaustics);
		ImGui::TooltipTextUnformatted(_("Removes certain paths that cause caustics"));
		changed += ImGui::Checkbox(_("High Quality BVH"), &params.highqualitybvh);
		ImGui::TooltipTextUnformatted(_("High quality bounding volume hierarchy"));
		ImGui::EndMenu();
	}

	if (changed > 0) {
		if (_pathTracer->writeAppearanceToScene(_sceneMgr->sceneGraph())) {
			_sceneMgr->markDirty();
		}
		_pathTracer->restart(sceneGraph, _sceneMgr->activeCamera());
	} else if (postprocessChanged) {
		if (_pathTracer->writeAppearanceToScene(_sceneMgr->sceneGraph())) {
			_sceneMgr->markDirty();
		}
		const image::ImagePtr img = _pathTracer->image();
		if (img && img->isLoaded()) {
			_image = img;
			_texture->upload(_image);
		}
	}
}

void RenderPanel::renderMenuBar(const scenegraph::SceneGraph &sceneGraph) {
	if (ImGui::BeginMenuBar()) {
		if (ImGui::BeginIconMenu(ICON_LC_SETTINGS, _("Settings"))) {
			renderSettings(sceneGraph);
			ImGui::EndMenu();
		}
		if (_image && _image->isLoaded()) {
			if (ImGui::IconMenuItem(ICON_LC_SAVE, _("Save image"))) {
				_app->saveDialog(
					[=](const core::String &file, const io::FormatDescription *desc) {
						const io::FilePtr &filePtr = _app->filesystem()->open(file, io::FileMode::SysWrite);
						io::FileStream stream(filePtr);
						image::writePNG(_image, stream);
					},
					{}, io::format::images(), "render.png");
			}
		}
		if (_pathTracer->started()) {
			if (ImGui::IconMenuItem(ICON_LC_REFRESH_CW, _("Sync camera"))) {
				_pathTracer->restart(sceneGraph, _sceneMgr->activeCamera());
				_pathTracer->state().params.camera = 0;
			}
			ImGui::TooltipTextUnformatted(_("Restart from the last focused viewport (what you were looking at)"));
			if (ImGui::IconMenuItem(ICON_LC_CIRCLE_STOP, _("Stop path tracer"))) {
				_pathTracer->stop();
			}
			const voxelpathtracer::PathTracerState &state = _pathTracer->state();
			const yocto::trace_params &params = state.params;
			ImGui::Text(_("Sample %i / %i"), _currentSample, params.samples);
			if (!state.backendMessage.empty()) {
				ImGui::TextWrapped("%s", state.backendMessage.c_str());
			}
			_pathTracer->update(&_currentSample);
			_image = _pathTracer->image();
			if (_image->isLoaded()) {
				_texture->upload(_image);
			}
		} else {
			if (ImGui::IconMenuItem(ICON_LC_PLAY, _("Start path tracer"))) {
				_currentSample = 0;
				_pathTracer->writeAppearanceToScene(_sceneMgr->sceneGraph());
				_pathTracer->start(sceneGraph, _sceneMgr->activeCamera());
				// Always begin from the live viewport camera. Scene camera
				// nodes and the Yocto fallback stay available in Settings.
				if (_sceneMgr->activeCamera() != nullptr) {
					_pathTracer->state().params.camera = 0;
				}
			}
			ImGui::TooltipTextUnformatted(_("Trace the last focused viewport camera"));
			const voxelpathtracer::PathTracerState &idleState = _pathTracer->state();
			if (_currentSample > 0 && _image && _image->isLoaded()) {
				if (_currentSample >= idleState.params.samples) {
					ImGui::Text(_("Done %i / %i"), _currentSample, idleState.params.samples);
				} else {
					ImGui::Text(_("Stopped %i / %i"), _currentSample, idleState.params.samples);
				}
				if (idleState.params.denoise) {
					ImGui::SameLine();
					ImGui::TextUnformatted(_("Denoise on"));
				}
			}
			if (!idleState.backendMessage.empty()) {
				ImGui::TextWrapped("%s", idleState.backendMessage.c_str());
			}
		}
		ImGui::EndMenuBar();
	}
}

void RenderPanel::syncFromScene(const scenegraph::SceneGraph &sceneGraph) {
	_pathTracer->applyAppearanceFromScene(sceneGraph);
}

void RenderPanel::flushToScene() {
	if (_pathTracer->writeAppearanceToScene(_sceneMgr->sceneGraph())) {
		_sceneMgr->markDirty();
	}
}

void RenderPanel::update(const char *id, const scenegraph::SceneGraph &sceneGraph) {
	core_trace_scoped(RenderPanel);
#if USE_YOCTO
	static ui::ScopedPanel panel(cfg::VoxEditShowRender);
	const core::String &title = makeTitle(ICON_LC_IMAGE, _("Render"), id);
	ui::ScopedPanel::Scope scope =
		panel.begin(title.c_str(), ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_MenuBar);
	if (!scope) {
		_pathTracer->stop();
		return;
	}
	renderMenuBar(sceneGraph);
	// TODO: allow to change the current scene camera like in the scene view in the Viewport class
	if (_texture->isLoaded()) {
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		const float texW = (float)_texture->width();
		const float texH = (float)_texture->height();
		if (texW > 0.0f && texH > 0.0f && avail.x > 0.0f && avail.y > 0.0f) {
			const float scaleX = avail.x / texW;
			const float scaleY = avail.y / texH;
			const float scale = scaleX < scaleY ? scaleX : scaleY;
			ImGui::Image(_texture->handle(), ImVec2(texW * scale, texH * scale));
		}
	}
#else
	(void)id;
	(void)sceneGraph;
#endif
}

void RenderPanel::shutdown() {
	if (_texture) {
		_texture->shutdown();
	}
	delete _pathTracer;
	_pathTracer = nullptr;
}

} // namespace voxedit
