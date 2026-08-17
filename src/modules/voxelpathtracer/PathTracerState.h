/**
 * @file
 */

#pragma once

#include "core/String.h"
#include <yocto_scene.h>
#include <yocto_trace.h>

namespace voxelpathtracer {

struct PathTracerState {
	yocto::trace_context context;
	yocto::scene_data scene;
	yocto::trace_bvh bvh;
	yocto::trace_params params;
	yocto::trace_lights lights;
	yocto::trace_state state;
	bool started = false;
	float aperture = 0.0f;
	float sunIntensity = 1.0f;
	float sunArea = 1.0f;
	float sunElevation = 55.0f * yocto::pif / 180.0f;
	float sunAzimuth = 135.0f * yocto::pif / 180.0f;
	bool sunDisk = false;
	bool skyEnvironment = false;
	bool hdriEnvironment = false;
	core::String hdriPath;
	float hdriIntensity = 1.0f;
	float hdriAzimuth = 0.0f;
	bool groundPlane = false;
	bool studioEdges = false;
	yocto::vec3f environmentColor = {0.91f, 0.91f, 0.92f};
	float exposure = 0.0f;
	bool filmic = false;
	// Set when WebGPU recovers or falls back. Shown in the Render panel so a
	// device-lost or exhausted GPU never becomes a silent black frame.
	core::String backendMessage;

	PathTracerState() : context(yocto::make_trace_context({})) {
		params.envhidden = true;
	}

	void resetAppearance() {
		params = yocto::trace_params();
		params.envhidden = true;
		aperture = 0.0f;
		sunIntensity = 1.0f;
		sunArea = 1.0f;
		sunElevation = 55.0f * yocto::pif / 180.0f;
		sunAzimuth = 135.0f * yocto::pif / 180.0f;
		sunDisk = false;
		skyEnvironment = false;
		hdriEnvironment = false;
		hdriPath = "";
		hdriIntensity = 1.0f;
		hdriAzimuth = 0.0f;
		groundPlane = false;
		studioEdges = false;
		environmentColor = {0.91f, 0.91f, 0.92f};
		exposure = 0.0f;
		filmic = false;
	}
};

} // namespace voxelpathtracer
