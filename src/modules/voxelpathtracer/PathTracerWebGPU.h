/**
 * @file
 */

#pragma once

#include "PathTracerScene.h"
#include "PathTracerPrimary.h"
#include "PathTracerTraversal.h"
#include "core/String.h"
#include <stdint.h>

namespace voxelpathtracer {

enum class PathTracerWebGPUState : uint8_t {
	Unavailable,
	Initializing,
	Ready,
	Failed
};

enum class PathTracerWebGPUEvent : uint8_t {
	None,
	DeviceLost,
	Recovered,
	RecoverFailed,
	ResourceExhausted,
	DispatchRejected,
	ReadbackFailed,
	InvalidAccumulation
};

// Portable device-loss / exhaustion policy. The first recoverable loss
// restarts GPU work; a second loss or a hard failure falls back to CPU
// once with a user-visible message. No silent black frame.
struct PathTracerWebGPULifecycle {
	uint32_t recoveries = 0u;
	uint32_t maxRecoveries = 1u;
	bool enabled = true;
	bool invalidRetry = false;
	bool resetAccumulation = false;
	bool needsUpload = false;
	core::String message;
};

const char *pathTracerWebGPUEventMessage(PathTracerWebGPUEvent event, bool recovered);
bool pathTracerWebGPUApplyEvent(PathTracerWebGPULifecycle &lifecycle, PathTracerWebGPUEvent event);

/**
 * Browser WebGPU device and buffer lifecycle for the portable traversal
 * kernel. The desktop build is an unavailable stub and keeps the CPU path as
 * the reference implementation.
 *
 * Dispatch is asynchronous. Call update(), then takeResults() after busy()
 * becomes false. A new dispatch is rejected until the previous result is
 * consumed.
 */
class PathTracerWebGPU {
private:
	int _handle = -1;
	PathTracerWebGPUState _state = PathTracerWebGPUState::Unavailable;
	uint32_t _gridCount = 0u;
	uint32_t _emitterCount = 0u;
	bool _needsUpload = false;
	core::String _fallbackMessage;

public:
	PathTracerWebGPU() = default;
	~PathTracerWebGPU();
	PathTracerWebGPU(const PathTracerWebGPU &) = delete;
	PathTracerWebGPU &operator=(const PathTracerWebGPU &) = delete;

	bool init();
	// Drop in-flight work and scene buffers. Keep the device so a later
	// start() can upload and dispatch without requestDevice() again.
	void abort();
	void shutdown();
	void update();

	inline PathTracerWebGPUState state() const {
		return _state;
	}
	inline bool ready() const {
		return _state == PathTracerWebGPUState::Ready;
	}
	inline bool needsUpload() const {
		return _needsUpload;
	}
	inline const core::String &fallbackMessage() const {
		return _fallbackMessage;
	}

	bool uploadScene(const PathTracerScene &scene);
	bool uploadEnvironment(const float *rgba, const float *cdf, uint32_t width, uint32_t height, float cdfSum);
	bool dispatch(const PathTracerRay *rays, uint32_t rayCount);
	bool dispatchPrimary(const PathTracerCameraData &camera, const PathTracerPrimaryParams &params,
						 const PathTracerLightingData &lighting, uint32_t sampleCount = 1u,
						 bool readHits = true);
	bool busy() const;
	bool takeResults(PathTracerVoxelHit *hits, uint32_t capacity, uint32_t &hitCount);
	// Returns cumulative radiance/guide sums. Dispatch sample zero resets the
	// persistent GPU accumulation buffer; sequential later samples add to it.
	bool takePrimaryResults(PathTracerVoxelHit *hits, PathTracerSampleOutput *outputs, uint32_t capacity,
							uint32_t &pixelCount);
	bool takePrimaryOutputs(PathTracerSampleOutput *outputs, uint32_t capacity, uint32_t &pixelCount);
};

} // namespace voxelpathtracer
