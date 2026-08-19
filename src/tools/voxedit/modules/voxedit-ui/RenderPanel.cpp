/**
 * @file
 */

#include "RenderPanel.h"
#include "core/SharedPtr.h"
#include "core/Var.h"
#include "io/FileStream.h"
#include "io/FormatDescription.h"
#include "scenegraph/SceneGraph.h"
#include "ui/IMGUIApp.h"
#include "ui/IMGUIEx.h"
#include "ui/IconsLucide.h"
#include "ui/ScopedPanel.h"
#include "ui/dearimgui/ImGuizmo.h"
#include "video/Camera.h"
#include "video/Texture.h"
#ifdef __EMSCRIPTEN__
#include "io/system/emscripten_browser_file.h"
#endif
#include "voxedit-ui/CameraPanel.h"
#include "voxedit-util/Config.h"
#include "voxedit-util/SceneManager.h"
#include "voxelpathtracer/IPathTracer.h"
#include "voxelpathtracer/PathTracerState.h"
#include "voxelrender/CameraMovement.h"
#include "voxelrender/RenderUtil.h"
#include <glm/gtc/type_ptr.hpp>
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/matrix_decompose.hpp>

namespace voxedit {

static bool s_hideAxis[3]{false, false, false};

RenderPanel::RenderPanel(ui::IMGUIApp *app, const SceneManagerPtr &sceneMgr)
	: Super(app, "render"), _pathTracer(voxelpathtracer::createPathTracer()), _sceneMgr(sceneMgr) {
	_camera.setRotationType(video::CameraRotationType::Target);
	_camera.setMode(video::CameraMode::Perspective);
}

bool RenderPanel::init() {
	_texture = video::createEmptyTexture("pathtracer");
	_viewDistance = core::getVar(cfg::VoxEditViewdistance);
	_gizmoAllowAxisFlip = core::getVar(cfg::VoxEditGizmoAllowAxisFlip);
	return true;
}

void RenderPanel::resetCamera() {
	const scenegraph::SceneGraph &sceneGraph = _sceneMgr->sceneGraph();
	const voxel::Region region = sceneGraph.sceneRegion(_sceneMgr->currentFrame(), true);
	const video::CameraRotationType rotationType = _camera.rotationType();
	voxelrender::SceneCameraMode cameraMode = _camMode;
	const float farPlane = _viewDistance ? _viewDistance->floatVal() : 5000.0f;
	voxelrender::configureCamera(_camera, region, cameraMode, farPlane);
	_camera.setRotationType(rotationType);
	_cameraInitialized = true;
}

void RenderPanel::setCamMode(voxelrender::SceneCameraMode mode) {
	_camMode = mode;
	resetCamera();
}

void RenderPanel::renderCameraManipulator(video::Camera &camera, float headerSize) {
	if (_camMode != voxelrender::SceneCameraMode::Free) {
		return;
	}
	ImVec2 position = ImGui::GetWindowPos();
	const ImVec2 size = ImVec2(128, 128);
	const ImVec2 available = ImGui::GetContentRegionAvail();
	const float contentRegionWidth = available.x + ImGui::GetCursorPosX();
	position.x += contentRegionWidth - size.x;
	position.y += headerSize;
	const ImU32 backgroundColor = 0;
	const float length = camera.targetDistance();

	glm::mat4 viewMatrix = camera.viewMatrix();
	float *viewPtr = glm::value_ptr(viewMatrix);

	const float *projPtr = glm::value_ptr(camera.projectionMatrix());
	const ImGuizmo::OPERATION operation = (ImGuizmo::OPERATION)0;
	glm::mat4 transformMatrix = glm::mat4(1.0f);
	float *matrixPtr = glm::value_ptr(transformMatrix);
	const ImGuizmo::MODE mode = ImGuizmo::MODE::LOCAL;
	ImGuizmo::ViewManipulate(viewPtr, projPtr, operation, mode, matrixPtr, length, position, size, backgroundColor);

	if (ImGuizmo::IsViewManipulateHovered()) {
		_cameraManipulated = true;
	}
	if (viewMatrix != camera.viewMatrix()) {
		glm::vec3 scale;
		glm::vec3 translation;
		glm::quat orientation;
		glm::vec3 skew;
		glm::vec4 perspective;
		glm::decompose(viewMatrix, scale, orientation, translation, skew, perspective);
		camera.setOrientation(orientation);
	}
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
						_pathTracer->restart(_sceneMgr->sceneGraph(), &_camera);
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
		_pathTracer->restart(sceneGraph, &_camera);
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
		if (ImGui::BeginIconMenu(ICON_LC_EYE, _("View"))) {
			if (ImGui::IconMenuItem(ICON_LC_VIDEO, _("Reset camera"))) {
				resetCamera();
			}
			ImGui::TooltipTextUnformatted(_("Reset the render camera to frame the scene"));
			if (ImGui::IconMenuItem(ICON_LC_REFRESH_CW, _("Sync from viewport"))) {
				if (const video::Camera *activeCam = _sceneMgr->activeCamera()) {
					_camera = *activeCam;
					_camera.setRotationType(video::CameraRotationType::Target);
					_camera.update(0.0);
					_cameraInitialized = true;
				}
			}
			ImGui::TooltipTextUnformatted(_("Copy camera pose from the active 3D edit viewport"));
			CameraPanel::cameraProjectionCombo(_camera);
			ImGui::EndMenu();
		}
		if (_image && _image->isLoaded()) {
			if (ImGui::IconMenuItem(ICON_LC_SAVE, _("Save image"))) {
				_app->saveDialog(
					[this](const core::String &file, const io::FormatDescription *desc) {
						if (_image && _image->isLoaded()) {
							const io::FilePtr &filePtr = io::filesystem()->open(file, io::FileMode::SysWrite);
							if (filePtr) {
								io::FileStream stream(filePtr);
								_image->writePNG(stream);
							}
						#ifdef __EMSCRIPTEN__
							const io::FilePtr &savedFile = _app->filesystem()->open(file, io::FileMode::SysRead);
							if (savedFile && savedFile->exists()) {
								uint8_t *buf = nullptr;
								const int len = savedFile->read((void **)&buf);
								if (buf != nullptr && len > 0) {
									const core::String &downloadName = core::string::extractFilenameWithExtension(file);
									emscripten_browser_file::download(downloadName.c_str(), "image/png", std::string_view((char const *)buf, (size_t)len), false);
								}
								SDL_free(buf);
								savedFile->close();
							}
						#endif
						}
					},
					{}, io::format::images(), "render.png");
			}
		}
		if (_pathTracer->started()) {
			if (ImGui::IconMenuItem(ICON_LC_REFRESH_CW, _("Sync camera"))) {
				if (const video::Camera *activeCam = _sceneMgr->activeCamera()) {
					_camera = *activeCam;
					_camera.setRotationType(video::CameraRotationType::Target);
					_camera.update(0.0);
					_cameraInitialized = true;
				}
				_pathTracer->restart(sceneGraph, &_camera);
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
				startPathTracer();
			}
			ImGui::TooltipTextUnformatted(_("Trace using the current render viewport camera"));
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

void RenderPanel::startPathTracer() {
	_currentSample = 0;
	if (!_cameraInitialized) {
		if (const video::Camera *activeCam = _sceneMgr->activeCamera()) {
			_camera = *activeCam;
			_camera.setRotationType(video::CameraRotationType::Target);
			_camera.update(0.0);
			_cameraInitialized = true;
		} else {
			resetCamera();
		}
	}
	// Sync tracer state FROM the scene first, so the scene's saved HDRI /
	// envhidden / filmic properties are not overwritten by tracer defaults
	// before start() reads them back via applyAppearanceFromScene.
	_pathTracer->applyAppearanceFromScene(_sceneMgr->sceneGraph());
	_pathTracer->writeAppearanceToScene(_sceneMgr->sceneGraph());
	_pathTracer->start(_sceneMgr->sceneGraph(), &_camera);
	_pathTracer->state().params.camera = 0;
}

void RenderPanel::stopPathTracer() {
	_pathTracer->stop();
}

bool RenderPanel::setHdri(const core::String &filename) {
	if (filename.empty()) {
		return false;
	}
	voxelpathtracer::PathTracerState &st = _pathTracer->state();
	st.hdriEnvironment = true;
#ifdef __EMSCRIPTEN__
	st.hdriPath = core::string::extractFilenameWithExtension(filename);
#else
	st.hdriPath = filename;
#endif
	_pathTracer->writeAppearanceToScene(_sceneMgr->sceneGraph());
	_sceneMgr->markDirty();
	if (_pathTracer->started()) {
		_pathTracer->restart(_sceneMgr->sceneGraph(), &_camera);
	}
	Log::info("Set HDRI environment to '%s'", st.hdriPath.c_str());
	return true;
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

	if (!_cameraInitialized) {
		if (const video::Camera *activeCam = _sceneMgr->activeCamera()) {
			_camera = *activeCam;
			_camera.setRotationType(video::CameraRotationType::Target);
			_camera.update(0.0);
			_cameraInitialized = true;
		} else {
			resetCamera();
		}
	}

	if (_viewDistance) {
		_camera.setFarPlane(_viewDistance->floatVal());
	}

	renderMenuBar(sceneGraph);

	const ImVec2 cursorPos = ImGui::GetCursorPos();
	const float headerSize = cursorPos.y;
	const ImVec2 avail = ImGui::GetContentRegionAvail();

	if (avail.x > 0.0f && avail.y > 0.0f) {
		const glm::ivec2 intAvail((int)avail.x, (int)avail.y);
		_camera.setSize(intAvail);
	}
	_camera.update(_app->deltaFrameSeconds());

	const glm::mat4 prevViewMatrix = _camera.viewMatrix();

	// Render the path-traced image
	if (_texture->isLoaded()) {
		const float texW = (float)_texture->width();
		const float texH = (float)_texture->height();
		if (texW > 0.0f && texH > 0.0f && avail.x > 0.0f && avail.y > 0.0f) {
			const float scaleX = avail.x / texW;
			const float scaleY = avail.y / texH;
			const float scale = scaleX < scaleY ? scaleX : scaleY;
			const ImVec2 renderImgSize(texW * scale, texH * scale);
			ImGui::Image(_texture->handle(), renderImgSize);
		}
	}

	// ImGuizmo view cube manipulator
	_cameraManipulated = false;
	ImGuizmo::PushID("render_viewport");
	ImGuizmo::SetDrawlist();
	ImGuizmo::SetWindow();
	const ImVec2 windowPos = ImGui::GetWindowPos();
	if (_gizmoAllowAxisFlip) {
		ImGuizmo::AllowAxisFlip(_gizmoAllowAxisFlip->boolVal());
	}
	ImGuizmo::SetAxisMask(s_hideAxis[0], s_hideAxis[1], s_hideAxis[2]);
	ImGuizmo::SetRect(windowPos.x, windowPos.y + headerSize, avail.x, avail.y);
	ImGuizmo::SetOrthographic(_camera.isOrthographic());
	renderCameraManipulator(_camera, headerSize);
	ImGuizmo::PopID();

	const bool gizmoActive = ImGuizmo::IsOver() || ImGuizmo::IsUsing() || ImGuizmo::IsUsingAny() ||
							 ImGuizmo::IsViewManipulateHovered() || ImGuizmo::IsUsingViewManipulate();
	_sceneMgr->setViewportGizmoActive(gizmoActive);

	// Interactive camera navigation on the render panel viewport (when not interacting with view cube)
	if (!gizmoActive) {
		const ImVec2 mousePos = ImGui::GetMousePos();
		const bool isHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
							   mousePos.x >= windowPos.x && mousePos.x <= windowPos.x + avail.x &&
							   mousePos.y >= windowPos.y + headerSize && mousePos.y <= windowPos.y + headerSize + avail.y;

		if (isHovered) {
			_sceneMgr->setActiveCamera(&_camera, false);
			const ImGuiIO &io = ImGui::GetIO();
			if (io.MouseWheel != 0.0f) {
				_sceneMgr->cameraMovement().zoom(_camera, -io.MouseWheel * 10.0f);
			}
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
				const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
				ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
				_sceneMgr->cameraMovement().rotate(_camera, delta.x, delta.y);
			}
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
				const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
				ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
				_sceneMgr->cameraMovement().pan(_camera, (int)delta.x, (int)delta.y);
			}
		}
	}

	// Detect camera movement and restart path tracing immediately
	_camera.update(0.0);
	if (_camera.viewMatrix() != prevViewMatrix) {
		if (scenegraph::SceneGraphNodeCamera *activeCam = _sceneMgr->activeCameraNode()) {
			const scenegraph::KeyFrameIndex keyFrameIdx = activeCam->keyFrameForFrame(_sceneMgr->currentFrame());
			voxelrender::applyCameraToNode(_camera, *activeCam, keyFrameIdx);
			scenegraph::SceneGraphTransform &transform = activeCam->transform(keyFrameIdx);
			if (transform.dirty()) {
				transform.update(_sceneMgr->sceneGraph(), *activeCam, activeCam->keyFrame(keyFrameIdx).frameIdx, false);
				_sceneMgr->sceneGraph().markKeyFramesDirty(activeCam->id());
			}
		}
		if (_pathTracer->started()) {
			_currentSample = 0;
			_pathTracer->restart(sceneGraph, &_camera);
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
