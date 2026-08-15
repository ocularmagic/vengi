/**
 * @file
 */

#pragma once

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
	yocto::vec3f environmentColor = {0.91f, 0.91f, 0.92f};
	float exposure = 0.0f;
	bool filmic = false;

	PathTracerState() : context(yocto::make_trace_context({})) {
	}

	void resetAppearance() {
		params = yocto::trace_params();
		aperture = 0.0f;
		sunIntensity = 1.0f;
		sunArea = 1.0f;
		sunElevation = 55.0f * yocto::pif / 180.0f;
		sunAzimuth = 135.0f * yocto::pif / 180.0f;
		sunDisk = false;
		skyEnvironment = false;
		environmentColor = {0.91f, 0.91f, 0.92f};
		exposure = 0.0f;
		filmic = false;
	}
};

} // namespace voxelpathtracer
