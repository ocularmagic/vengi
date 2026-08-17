/**
 * @file
 */

#pragma once

#include "core/String.h"
#include "core/collection/Buffer.h"

namespace voxelpathtracer {

/**
 * Decode a Radiance .hdr (stb_image) or OpenEXR .exr (tinyexr) into a flat
 * float RGBA buffer (4 floats per pixel, alpha forced to 1). NaN, +/-inf and
 * negative channels are clamped to zero so the luminance CDF stays finite.
 * Returns false on any read/decode failure.
 */
bool pathTracerLoadHdriFloats(const core::String &path, core::Buffer<float> &rgba, int &width, int &height);

} // namespace voxelpathtracer
