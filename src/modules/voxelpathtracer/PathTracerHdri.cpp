/**
 * @file
 */

#include "PathTracerHdri.h"
#include "Appearance.h"
#include "core/Log.h"
#include "core/StandardLib.h"
#include "core/StringUtil.h"
#include "io/File.h"
#include <stdlib.h>
#include <stb_image.h>
#include "tinyexr.h"

namespace voxelpathtracer {

bool pathTracerLoadHdriFloats(const core::String &path, core::Buffer<float> &rgba, int &width, int &height) {
	width = 0;
	height = 0;
	rgba.release();
	const core::String &resolved = resolveHdriPath(path);
	if (resolved.empty()) {
		Log::error("HDRI file not found: %s", path.c_str());
		return false;
	}
	io::File file(resolved, io::FileMode::SysRead);
	void *buffer = nullptr;
	const int len = file.read(&buffer);
	if (len <= 0 || buffer == nullptr) {
		Log::error("Failed to read HDRI file: %s", path.c_str());
		delete[] (uint8_t *)buffer;
		return false;
	}
	if (core::string::endsWith(resolved, ".exr", true)) {
		const char *err = nullptr;
		float *pixels = nullptr;
		int w = 0;
		int h = 0;
		const int ret = LoadEXRFromMemory(&pixels, &w, &h, (const unsigned char *)buffer, (size_t)len, &err);
		delete[] (uint8_t *)buffer;
		if (ret != TINYEXR_SUCCESS || pixels == nullptr || w <= 0 || h <= 0) {
			Log::error("Failed to decode HDRI %s: %s", path.c_str(), err != nullptr ? err : "unknown error");
			if (pixels != nullptr) {
				free(pixels);
			}
			return false;
		}
		width = w;
		height = h;
		const int n = w * h;
		rgba.resize((size_t)n * 4u);
		for (int i = 0; i < n; ++i) {
			for (int c = 0; c < 3; ++c) {
				const float v = pixels[i * 4 + c];
				// EXR floats can be NaN, +/-inf, or negative; keep the CDF finite.
				rgba[i * 4 + c] = (!(v >= 0.0f) || v > 1.0e30f) ? 0.0f : v;
			}
			rgba[i * 4 + 3] = 1.0f;
		}
		free(pixels);
		return true;
	}
	int components = 0;
	float *pixels = stbi_loadf_from_memory((const stbi_uc *)buffer, len, &width, &height, &components, 4);
	delete[] (uint8_t *)buffer;
	if (pixels == nullptr || width <= 0 || height <= 0) {
		Log::error("Failed to decode HDRI %s: %s", path.c_str(), stbi_failure_reason());
		if (pixels != nullptr) {
			stbi_image_free(pixels);
		}
		return false;
	}
	rgba.resize((size_t)width * (size_t)height * 4u);
	core_memcpy(rgba.data(), pixels, (size_t)width * (size_t)height * 4u * sizeof(float));
	stbi_image_free(pixels);
	return true;
}

} // namespace voxelpathtracer
