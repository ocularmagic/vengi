/**
 * @file
 */

#include "PathTracerWebGPU.h"
#include "PathTracerTraversalWGSL.h"
#include <string.h>
#include <glm/gtc/matrix_inverse.hpp>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(int, vengiPathTracerWebGPUCreate, (const char *sourcePointer, uint32_t sourceLength), {
	if (typeof navigator === 'undefined' || !navigator.gpu) {
		return -1;
	}
	const runtime = Module.vengiPathTracerWebGPU || (Module.vengiPathTracerWebGPU = {
		nextHandle: 1,
		backends: new Map(),
		ready: Promise.resolve(),
		// Request a device with the storage-buffer limits the tracer needs.
		// The WebGPU DEFAULT maxStorageBufferBindingSize is only 128 MiB and
		// maxBufferSize 256 MiB. At 1280x1280 the primary-hit and accumulated
		// output buffers are ~150 MiB each, so with the defaults every
		// createBindGroup fails validation, the compute pass never runs, and
		// the readback stays all zero -> a fully black render with no CPU
		// fallback (the JS errors are uncaptured, not thrown). Raise the
		// limits to what the adapter actually reports.
		async requestDevice(adapter) {
			const wanted = ['maxStorageBufferBindingSize', 'maxBufferSize',
				'maxComputeWorkgroupStorageSize', 'maxComputeInvocationsPerWorkgroup'];
			const requiredLimits = {};
			for (const name of wanted) {
				const value = adapter.limits ? adapter.limits[name] : undefined;
				if (typeof value === 'number' && value > 0) {
					requiredLimits[name] = value;
				}
			}
			try {
				return await adapter.requestDevice({requiredLimits});
			} catch (error) {
				console.warn('Path tracer WebGPU: requiredLimits rejected, retrying with defaults:', error);
				return await adapter.requestDevice();
			}
		},
		destroyBuffer(record, name) {
			const buffer = record[name];
			if (!buffer) {
				return;
			}
			try {
				buffer.unmap();
			} catch (error) {
			}
			buffer.destroy();
			record[name] = null;
			if (record.bufferBytes) {
				delete record.bufferBytes[name];
				delete record.bufferUsage[name];
			}
		},
		ensureBuffer(record, name, size, usage, label) {
			const existing = record[name];
			if (existing && record.bufferBytes[name] === size && record.bufferUsage[name] === usage) {
				return existing;
			}
			this.destroyBuffer(record, name);
			const buffer = record.device.createBuffer({label, size, usage});
			record[name] = buffer;
			record.bufferBytes[name] = size;
			record.bufferUsage[name] = usage;
			return buffer;
		},
		dropBuffers(record) {
			record.generation += 1;
			for (const name of ['grids', 'cells', 'materials', 'emitters', 'ground', 'environmentTexels',
				'environmentCdf', 'environmentData', 'mediaData', 'rays', 'hits', 'params', 'camera',
				'primaryParams', 'lighting', 'sampleOutputs', 'readback', 'sampleReadback',
				'denoiseScratchA', 'denoiseScratchB', 'temporalHistoryA', 'temporalHistoryB',
				'finalImage', 'denoiseParams', 'denoiseCamera', 'denoisePreviousVP',
				'unconvergedCount', 'finalReadback', 'convergenceReadback']) {
				this.destroyBuffer(record, name);
			}
			record.gridCount = 0;
			record.emitterCount = 0;
			record.samplePixelCount = 0;
			record.temporalParity = 0;
		},
		fail(record, message) {
			record.state = 3;
			record.busy = false;
			record.result = null;
			record.sampleResult = null;
			record.resultRayCount = 0;
			record.fallbackMessage = message;
		},
		async attachDevice(record, device, source) {
			record.device = device;
			device.lost.then(info => {
				if (record.destroyed) {
					return;
				}
				console.error('Path tracer WebGPU device lost:', info.message);
				this.recover(record);
			});
			device.addEventListener('uncapturederror', event => {
				const error = event.error;
				const oom = error && error.constructor && error.constructor.name === 'GPUOutOfMemoryError';
				console.error('Path tracer WebGPU uncaptured error:', error && error.message);
				if (oom && !record.destroyed) {
					this.fail(record, 'WebGPU ran out of GPU memory; continuing on the CPU renderer');
				}
			});
			const module = device.createShaderModule({label: 'voxel path tracer traversal', code: source});
			const compilation = await module.getCompilationInfo();
			const errors = compilation.messages.filter(message => message.type === 'error');
			if (errors.length !== 0) {
				throw new Error(errors.map(error =>
					`${error.lineNum}:${error.linePos} ${error.message}`).join('\n'));
			}
			record.pipeline = await device.createComputePipelineAsync({
				label: 'voxel path tracer traversal',
				layout: 'auto',
				compute: {module, entryPoint: 'main'}
			});
			record.primaryPipeline = await device.createComputePipelineAsync({
				label: 'voxel path tracer primary rays',
				layout: 'auto',
				compute: {module, entryPoint: 'primaryMain'}
			});
			record.convergencePipeline = await device.createComputePipelineAsync({
				label: 'voxel convergence flag',
				layout: 'auto',
				compute: {module, entryPoint: 'convergenceMain'}
			});
			record.denoisePipeline = await device.createComputePipelineAsync({
				label: 'voxel denoise tonemap',
				layout: 'auto',
				compute: {module, entryPoint: 'denoiseMain'}
			});
		},
		async recover(record) {
			if (record.destroyed) {
				return false;
			}
			record.busy = false;
			record.result = null;
			record.sampleResult = null;
			record.resultRayCount = 0;
			this.dropBuffers(record);
			record.pipeline = null;
			record.primaryPipeline = null;
			record.convergencePipeline = null;
			record.denoisePipeline = null;
			if (record.device) {
				const lost = Promise.resolve(record.device.lost).catch(() => {});
				try {
					record.device.destroy();
				} catch (error) {
				}
				record.device = null;
				this.ready = Promise.resolve(this.ready).then(() => lost);
			}
			if (record.recoveries >= record.maxRecoveries) {
				this.fail(record, 'WebGPU device was lost; continuing on the CPU renderer');
				return false;
			}
			record.recoveries += 1;
			record.state = 1;
			try {
				await this.ready;
				if (record.destroyed) {
					return false;
				}
				const adapter = await navigator.gpu.requestAdapter();
				if (!adapter) {
					throw new Error('WebGPU adapter request failed after device loss');
				}
				const device = await this.requestDevice(adapter);
				if (record.destroyed) {
					device.destroy();
					this.ready = Promise.resolve(device.lost).catch(() => {});
					return false;
				}
				await this.attachDevice(record, device, record.source);
				if (record.destroyed) {
					return false;
				}
				record.needsUpload = true;
				record.state = 2;
				record.fallbackMessage = 'WebGPU device was lost; recovered and restarted the render';
				return true;
			} catch (error) {
				console.error('Path tracer WebGPU recovery failed:', error);
				this.fail(record, 'WebGPU device was lost; continuing on the CPU renderer');
				return false;
			}
		}
	});
	const handle = runtime.nextHandle++;
	const record = {
		state: 1,
		destroyed: false,
		device: null,
		pipeline: null,
		primaryPipeline: null,
		convergencePipeline: null,
		denoisePipeline: null,
		grids: null,
		cells: null,
		materials: null,
		emitters: null,
		ground: null,
		environmentTexels: null,
		environmentCdf: null,
		environmentData: null,
		mediaData: null,
		rays: null,
		hits: null,
		params: null,
		camera: null,
		primaryParams: null,
		lighting: null,
		sampleOutputs: null,
		sampleReadback: null,
		readback: null,
		bufferBytes: {},
		bufferUsage: {},
		gridCount: 0,
		emitterCount: 0,
		busy: false,
		result: null,
		sampleResult: null,
		denoiseResult: null,
		convergenceResult: 0,
		resultRayCount: 0,
		samplePixelCount: 0,
		temporalParity: 0,
		source: "",
		recoveries: 0,
		maxRecoveries: 1,
		generation: 0,
		needsUpload: false,
		fallbackMessage: ""
	};
	runtime.backends.set(handle, record);
	const source = UTF8ToString(sourcePointer, sourceLength);
	record.source = source;
	(async () => {
		await runtime.ready;
		if (record.destroyed) {
			return;
		}
		const adapter = await navigator.gpu.requestAdapter();
		if (!adapter) {
			throw new Error('WebGPU adapter request failed');
		}
		const device = await runtime.requestDevice(adapter);
		if (record.destroyed) {
			device.destroy();
			runtime.ready = Promise.resolve(device.lost).catch(() => {});
			return;
		}
		await runtime.attachDevice(record, device, source);
		if (!record.destroyed) {
			record.state = 2;
		}
	})().catch(error => {
		if (!record.destroyed) {
			runtime.fail(record, 'WebGPU is unavailable; using CPU path tracer');
			console.error('Path tracer WebGPU initialization failed:', error);
		}
	});
	return handle;
});

EM_JS(int, vengiPathTracerWebGPUState, (int handle), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	return record ? record.state : 0;
});

EM_JS(int, vengiPathTracerWebGPUConsumeNeedsUpload, (int handle), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	if (!record || !record.needsUpload) {
		return 0;
	}
	record.needsUpload = false;
	return 1;
});

EM_JS(int, vengiPathTracerWebGPUCopyMessage, (int handle, char *dst, uint32_t cap), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	const message = record && record.fallbackMessage ? record.fallbackMessage : "";
	if (!dst || cap === 0) {
		return message.length;
	}
	const n = Math.min(message.length, cap - 1);
	for (let i = 0; i < n; ++i) {
		HEAPU8[dst + i] = message.charCodeAt(i) & 0xff;
	}
	HEAPU8[dst + n] = 0;
	return message.length;
});

EM_JS(void, vengiPathTracerWebGPUDestroy, (int handle), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	if (!record) {
		return;
	}
	record.destroyed = true;
	record.busy = false;
	record.result = null;
	record.sampleResult = null;
	record.resultRayCount = 0;
	record.samplePixelCount = 0;
	record.gridCount = 0;
	record.emitterCount = 0;
	runtime.dropBuffers(record);
	if (record.device) {
		const lost = Promise.resolve(record.device.lost).catch(() => {});
		record.device.destroy();
		record.device = null;
		runtime.ready = Promise.resolve(runtime.ready).then(() => lost);
	}
	runtime.backends.delete(handle);
});

EM_JS(void, vengiPathTracerWebGPUAbort, (int handle), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	if (!record || record.destroyed) {
		return;
	}
	record.busy = false;
	record.result = null;
	record.sampleResult = null;
	record.resultRayCount = 0;
	record.samplePixelCount = 0;
	record.gridCount = 0;
	record.emitterCount = 0;
	runtime.dropBuffers(record);
});

EM_JS(int, vengiPathTracerWebGPUUploadScene,
	  (int handle, const void *gridPointer, uint32_t gridBytes, uint32_t gridCount,
	   const void *cellPointer, uint32_t cellBytes, const void *materialPointer, uint32_t materialBytes,
	   const void *emitterPointer, uint32_t emitterBytes, uint32_t emitterCount,
	   const void *groundPointer, const void *mediaPointer), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	if (!record || record.state !== 2 || record.busy || record.result || record.sampleResult || gridCount === 0 ||
		gridBytes === 0 || cellBytes === 0 || materialBytes === 0 ||
		(emitterCount !== 0 && emitterBytes === 0)) {
		return 0;
	}
	try {
		const storage = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST;
		const uniform = GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST;
		const write = (name, pointer, byteLength, usage, label) => {
			const buffer = runtime.ensureBuffer(record, name, byteLength, usage, label);
			record.device.queue.writeBuffer(buffer, 0, HEAPU8.slice(pointer, pointer + byteLength));
			return buffer;
		};
		write('grids', gridPointer, gridBytes, storage, 'path tracer grids');
		write('cells', cellPointer, cellBytes, storage, 'path tracer cells');
		write('materials', materialPointer, materialBytes, storage, 'path tracer materials');
		if (emitterCount !== 0) {
			write('emitters', emitterPointer, emitterBytes, storage, 'path tracer emitters');
		} else {
			runtime.ensureBuffer(record, 'emitters', 96, storage, 'path tracer empty emitter');
		}
		write('ground', groundPointer, 48, uniform, 'path tracer ground');
		write('mediaData', mediaPointer, 32, uniform, 'path tracer media data');
		if (!record.environmentTexels) {
			runtime.ensureBuffer(record, 'environmentTexels', 16, storage, 'path tracer empty environment texel');
			runtime.ensureBuffer(record, 'environmentCdf', 4, storage, 'path tracer empty environment cdf');
			record.environmentData = runtime.ensureBuffer(record, 'environmentData', 32, uniform,
				'path tracer empty environment data');
			record.device.queue.writeBuffer(record.environmentData, 0,
				new Uint32Array([1, 1, 1, 0, 0, 0, 0, 0]));
		}
		// A new scene invalidates progressive GPU accumulation.
		runtime.destroyBuffer(record, 'sampleOutputs');
		runtime.destroyBuffer(record, 'sampleReadback');
		record.samplePixelCount = 0;
		record.gridCount = gridCount;
		record.emitterCount = emitterCount;
		return 1;
	} catch (error) {
		runtime.fail(record, 'WebGPU ran out of GPU memory; continuing on the CPU renderer');
		console.error('Path tracer WebGPU scene upload failed:', error);
		return 0;
	}
});

EM_JS(int, vengiPathTracerWebGPUUploadEnvironment,
	  (int handle, const void *rgbaPointer, const void *cdfPointer, uint32_t width, uint32_t height,
	   float cdfSum), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	const texelCount = width * height;
	if (!record || record.state !== 2 || record.busy || record.result || record.sampleResult ||
		texelCount === 0 || !Number.isFinite(cdfSum) || cdfSum <= 0) {
		return 0;
	}
	try {
		const storage = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST;
		const uniform = GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST;
		const texelBuffer = runtime.ensureBuffer(record, 'environmentTexels', texelCount * 16, storage,
			'path tracer environment texels');
		record.device.queue.writeBuffer(texelBuffer, 0,
			HEAPU8.slice(rgbaPointer, rgbaPointer + texelCount * 16));
		const cdfBuffer = runtime.ensureBuffer(record, 'environmentCdf', texelCount * 4, storage,
			'path tracer environment cdf');
		record.device.queue.writeBuffer(cdfBuffer, 0,
			HEAPU8.slice(cdfPointer, cdfPointer + texelCount * 4));
		const data = new ArrayBuffer(32);
		const dataView = new DataView(data);
		dataView.setUint32(0, width, true);
		dataView.setUint32(4, height, true);
		dataView.setUint32(8, texelCount, true);
		dataView.setUint32(12, 1, true);
		dataView.setFloat32(16, cdfSum, true);
		const envData = runtime.ensureBuffer(record, 'environmentData', 32, uniform,
			'path tracer environment data');
		record.device.queue.writeBuffer(envData, 0, data);
		return 1;
	} catch (error) {
		runtime.fail(record, 'WebGPU ran out of GPU memory; continuing on the CPU renderer');
		console.error('Path tracer WebGPU environment upload failed:', error);
		return 0;
	}
});

EM_JS(int, vengiPathTracerWebGPUDispatch, (int handle, const void *rayPointer, uint32_t rayCount), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	if (!record || record.state !== 2 || record.busy || record.result || record.sampleResult ||
		record.gridCount === 0 || rayCount === 0) {
		return 0;
	}
	try {
		const rayBytes = rayCount * 64;
		const hitBytes = rayCount * 96;
		const rays = runtime.ensureBuffer(record, 'rays', rayBytes,
			GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST, 'path tracer rays');
		record.device.queue.writeBuffer(rays, 0, HEAPU8.slice(rayPointer, rayPointer + rayBytes));
		const hits = runtime.ensureBuffer(record, 'hits', hitBytes,
			GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC, 'path tracer hits');
		const params = runtime.ensureBuffer(record, 'params', 16,
			GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST, 'path tracer dispatch params');
		record.device.queue.writeBuffer(params, 0,
			new Uint32Array([rayCount, record.gridCount, 0, 0]));
		const readback = runtime.ensureBuffer(record, 'readback', hitBytes,
			GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ, 'path tracer hit readback');
		const bindGroup = record.device.createBindGroup({
			layout: record.pipeline.getBindGroupLayout(0),
			entries: [
				{binding: 0, resource: {buffer: record.grids}},
				{binding: 1, resource: {buffer: record.cells}},
				{binding: 2, resource: {buffer: record.materials}},
				{binding: 3, resource: {buffer: record.rays}},
				{binding: 4, resource: {buffer: record.hits}},
				{binding: 5, resource: {buffer: record.params}}
			]
		});
		const encoder = record.device.createCommandEncoder();
		const pass = encoder.beginComputePass();
		pass.setPipeline(record.pipeline);
		pass.setBindGroup(0, bindGroup);
		pass.dispatchWorkgroups(Math.ceil(rayCount / 64));
		pass.end();
		encoder.copyBufferToBuffer(hits, 0, readback, 0, hitBytes);
		record.device.queue.submit([encoder.finish()]);
		record.busy = true;
		const generation = record.generation;
		readback.mapAsync(GPUMapMode.READ).then(() => {
			if (record.destroyed || generation !== record.generation || record.state !== 2) {
				return;
			}
			record.result = new Uint8Array(readback.getMappedRange()).slice();
			record.resultRayCount = rayCount;
			readback.unmap();
			record.busy = false;
		}).catch(error => {
			if (record.destroyed || generation !== record.generation) {
				return;
			}
			if (record.state === 1) {
				record.busy = false;
				return;
			}
			runtime.fail(record, 'WebGPU readback failed; continuing on the CPU renderer');
			console.error('Path tracer WebGPU readback failed:', error);
		});
		return 1;
	} catch (error) {
		runtime.fail(record, 'WebGPU dispatch was rejected; continuing on the CPU renderer');
		console.error('Path tracer WebGPU dispatch failed:', error);
		return 0;
	}
});

EM_JS(int, vengiPathTracerWebGPUDispatchPrimary,
	  (int handle, const void *cameraPointer, const void *paramsPointer,
	   const void *lightingPointer, uint32_t pixelCount, uint32_t sampleCount, int readHits), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	if (!record || record.state !== 2 || record.busy || record.result || record.sampleResult ||
		record.gridCount === 0 || pixelCount === 0 || sampleCount === 0) {
		return 0;
	}
	try {
		const hitBytes = pixelCount * 96;
		const outputBytes = pixelCount * 96;
		const paramsBytes = HEAPU8.slice(paramsPointer, paramsPointer + 32);
		const baseParams = new Uint32Array(paramsBytes.buffer, paramsBytes.byteOffset, 8);
		const sampleIndex = baseParams[1];
		const hitUsage = GPUBufferUsage.STORAGE | (readHits ? GPUBufferUsage.COPY_SRC : 0);
		runtime.ensureBuffer(record, 'hits', hitBytes, hitUsage, 'path tracer primary hits');
		const camera = runtime.ensureBuffer(record, 'camera', 80,
			GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST, 'path tracer camera');
		record.device.queue.writeBuffer(camera, 0, HEAPU8.slice(cameraPointer, cameraPointer + 80));
		const uniformStride = 256;
		const primaryParams = runtime.ensureBuffer(record, 'primaryParams', uniformStride * sampleCount,
			GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST, 'path tracer primary params');
		for (let sampleOffset = 0; sampleOffset < sampleCount; ++sampleOffset) {
			const sampleParams = new Uint32Array(baseParams);
			sampleParams[1] = sampleIndex + sampleOffset;
			record.device.queue.writeBuffer(primaryParams, uniformStride * sampleOffset, sampleParams);
		}
		const lighting = runtime.ensureBuffer(record, 'lighting', 64,
			GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST, 'path tracer lighting');
		record.device.queue.writeBuffer(lighting, 0, HEAPU8.slice(lightingPointer, lightingPointer + 64));
		const outputUsage = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST;
		if (!record.sampleOutputs || record.samplePixelCount !== pixelCount) {
			runtime.ensureBuffer(record, 'sampleOutputs', outputBytes, outputUsage,
				'path tracer accumulated outputs');
			record.device.queue.writeBuffer(record.sampleOutputs, 0, new Uint8Array(outputBytes));
			record.samplePixelCount = pixelCount;
		} else if (sampleIndex === 0) {
			record.device.queue.writeBuffer(record.sampleOutputs, 0, new Uint8Array(outputBytes));
		}
		if (readHits) {
			runtime.ensureBuffer(record, 'readback', hitBytes,
				GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ, 'path tracer primary readback');
		} else {
			runtime.destroyBuffer(record, 'readback');
		}
		const sampleReadback = runtime.ensureBuffer(record, 'sampleReadback', outputBytes,
			GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ, 'path tracer accumulated readback');
		const encoder = record.device.createCommandEncoder();
		for (let sampleOffset = 0; sampleOffset < sampleCount; ++sampleOffset) {
			const bindGroup = record.device.createBindGroup({
				layout: record.primaryPipeline.getBindGroupLayout(0),
				entries: [
					{binding: 0, resource: {buffer: record.grids}},
					{binding: 1, resource: {buffer: record.cells}},
					{binding: 2, resource: {buffer: record.materials}},
					{binding: 4, resource: {buffer: record.hits}},
					{binding: 6, resource: {buffer: record.camera}},
					{binding: 7, resource: {buffer: record.primaryParams,
						offset: uniformStride * sampleOffset, size: 32}},
					{binding: 8, resource: {buffer: record.lighting}},
					{binding: 9, resource: {buffer: record.sampleOutputs}},
					{binding: 10, resource: {buffer: record.emitters}},
					{binding: 11, resource: {buffer: record.ground}},
					{binding: 12, resource: {buffer: record.environmentTexels}},
					{binding: 13, resource: {buffer: record.environmentCdf}},
					{binding: 14, resource: {buffer: record.environmentData}},
					{binding: 15, resource: {buffer: record.mediaData}}
				]
			});
			const pass = encoder.beginComputePass();
			pass.setPipeline(record.primaryPipeline);
			pass.setBindGroup(0, bindGroup);
			pass.dispatchWorkgroups(Math.ceil(pixelCount / 64));
			pass.end();
		}
		if (readHits) {
			encoder.copyBufferToBuffer(record.hits, 0, record.readback, 0, hitBytes);
		}
		encoder.copyBufferToBuffer(record.sampleOutputs, 0, sampleReadback, 0, outputBytes);
		record.device.queue.submit([encoder.finish()]);
		record.busy = true;
		const generation = record.generation;
		const mappings = [sampleReadback.mapAsync(GPUMapMode.READ)];
		if (readHits) {
			mappings.push(record.readback.mapAsync(GPUMapMode.READ));
		}
		Promise.all(mappings).then(() => {
			if (record.destroyed || generation !== record.generation || record.state !== 2) {
				return;
			}
			if (readHits) {
				record.result = new Uint8Array(record.readback.getMappedRange()).slice();
				record.readback.unmap();
			}
			record.sampleResult = new Uint8Array(sampleReadback.getMappedRange()).slice();
			record.resultRayCount = pixelCount;
			sampleReadback.unmap();
			record.busy = false;
		}).catch(error => {
			if (record.destroyed || generation !== record.generation) {
				return;
			}
			if (record.state === 1) {
				record.busy = false;
				return;
			}
			runtime.fail(record, 'WebGPU readback failed; continuing on the CPU renderer');
			console.error('Path tracer WebGPU primary readback failed:', error);
		});
		return 1;
	} catch (error) {
		runtime.fail(record, 'WebGPU dispatch was rejected; continuing on the CPU renderer');
		console.error('Path tracer WebGPU primary dispatch failed:', error);
		return 0;
	}
});

EM_JS(int, vengiPathTracerWebGPUBusy, (int handle), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	return record && (record.busy || record.result || record.sampleResult) ? 1 : 0;
});

EM_JS(int, vengiPathTracerWebGPUTakePrimaryResults,
	  (int handle, void *hitPointer, void *outputPointer, uint32_t capacity), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	if (!record || !record.result || !record.sampleResult) {
		return 0;
	}
	if (capacity < record.resultRayCount) {
		return -record.resultRayCount;
	}
	HEAPU8.set(record.result, hitPointer);
	HEAPU8.set(record.sampleResult, outputPointer);
	const count = record.resultRayCount;
	record.result = null;
	record.sampleResult = null;
	record.resultRayCount = 0;
	return count;
});

EM_JS(int, vengiPathTracerWebGPUTakePrimaryOutputs,
	  (int handle, void *outputPointer, uint32_t capacity), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	if (!record || !record.sampleResult) {
		return 0;
	}
	if (capacity < record.resultRayCount) {
		return -record.resultRayCount;
	}
	HEAPU8.set(record.sampleResult, outputPointer);
	const count = record.resultRayCount;
	record.sampleResult = null;
	record.resultRayCount = 0;
	return count;
});

EM_JS(int, vengiPathTracerWebGPURenderBatch,
	  (int handle, const void *cameraPointer, const void *paramsPointer,
	   const void *lightingPointer, uint32_t sampleCount, uint32_t width, uint32_t height,
	   float exposure, int filmic, int seed, const void *previousVPPointer), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	if (!record || record.state !== 2 || record.busy || record.result || record.sampleResult ||
		record.gridCount === 0 || sampleCount === 0 || width === 0 || height === 0) {
		return 0;
	}
	try {
		const pixelCount = width * height;
		const outputBytes = pixelCount * 96;
		const scratchBytes = pixelCount * 16;
		const historyBytes = pixelCount * 64;
		const imageBytes = pixelCount * 4;
		const storage = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST;

		// Accumulation buffers: sampleOutputs, camera, strided primary params,
		// and lighting.
		const paramsBytes = HEAPU8.slice(paramsPointer, paramsPointer + 32);
		const baseParams = new Uint32Array(paramsBytes.buffer, paramsBytes.byteOffset, 8);
		const sampleIndex = baseParams[1];
		runtime.ensureBuffer(record, 'hits', outputBytes, GPUBufferUsage.STORAGE, 'path tracer primary hits');
		const camera = runtime.ensureBuffer(record, 'camera', 80,
			GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST, 'path tracer camera');
		record.device.queue.writeBuffer(camera, 0, HEAPU8.slice(cameraPointer, cameraPointer + 80));
		const uniformStride = 256;
		const primaryParams = runtime.ensureBuffer(record, 'primaryParams', uniformStride * sampleCount,
			GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST, 'path tracer primary params');
		for (let sampleOffset = 0; sampleOffset < sampleCount; ++sampleOffset) {
			const sampleParams = new Uint32Array(baseParams);
			sampleParams[1] = sampleIndex + sampleOffset;
			record.device.queue.writeBuffer(primaryParams, uniformStride * sampleOffset, sampleParams);
		}
		const lighting = runtime.ensureBuffer(record, 'lighting', 64,
			GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST, 'path tracer lighting');
		record.device.queue.writeBuffer(lighting, 0, HEAPU8.slice(lightingPointer, lightingPointer + 64));
		if (!record.sampleOutputs || record.samplePixelCount !== pixelCount) {
			runtime.ensureBuffer(record, 'sampleOutputs', outputBytes,
				GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST,
				'path tracer accumulated outputs');
			record.device.queue.writeBuffer(record.sampleOutputs, 0, new Uint8Array(outputBytes));
			record.samplePixelCount = pixelCount;
		} else if (sampleIndex === 0) {
			record.device.queue.writeBuffer(record.sampleOutputs, 0, new Uint8Array(outputBytes));
		}

		// Denoise + convergence buffers.
		const scratchA = runtime.ensureBuffer(record, 'denoiseScratchA', scratchBytes, storage, 'denoise scratch A');
		const scratchB = runtime.ensureBuffer(record, 'denoiseScratchB', scratchBytes, storage, 'denoise scratch B');
		const historyA = runtime.ensureBuffer(record, 'temporalHistoryA', historyBytes, storage, 'temporal history A');
		const historyB = runtime.ensureBuffer(record, 'temporalHistoryB', historyBytes, storage, 'temporal history B');
		const finalImage = runtime.ensureBuffer(record, 'finalImage', imageBytes,
			GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC, 'final image');
		// One uniform SLOT PER PASS. queue.writeBuffer is ordered on the queue,
		// NOT interleaved with the encoder's compute passes: writing a single
		// 48-byte denoiseParams buffer between beginComputePass() calls that
		// share one encoder makes ALL passes observe the LAST write (verified
		// in isolation). That collapsed init/a-trous/temporal into the tonemap
		// mode, so the tonemap read an all-zero scratch buffer and the render
		// came out RGB 0 with alpha 255 -- a black image despite correct
		// radiance in sampleOutputs. Give every pass its own 256-byte-aligned
		// slot and bind with a dynamic-free explicit offset, exactly like
		// primaryParams above.
		const denoisePassPlan = [
			// mode, step, seedFlag, ping, pong
			[0, 0, 0, scratchB, scratchA], // init    -> scratchA
			[1, 1, 0, scratchA, scratchB], // a-trous -> scratchB
			[1, 2, 0, scratchB, scratchA], // a-trous -> scratchA
			[1, 4, 0, scratchA, scratchB], // a-trous -> scratchB
			[3, 0, seed, scratchB, scratchA], // temporal -> scratchA
			[2, 0, 0, scratchA, scratchB]  // tonemap  <- scratchA
		];
		const denoiseParams = runtime.ensureBuffer(record, 'denoiseParams',
			uniformStride * denoisePassPlan.length,
			GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST, 'denoise params');
		const denoiseCamera = runtime.ensureBuffer(record, 'denoiseCamera', 80,
			GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST, 'denoise camera');
		const denoisePreviousVP = runtime.ensureBuffer(record, 'denoisePreviousVP', 64,
			GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST, 'denoise previous vp');
		const unconvergedCount = runtime.ensureBuffer(record, 'unconvergedCount', 4,
			GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST, 'unconverged count');
		const finalReadback = runtime.ensureBuffer(record, 'finalReadback', imageBytes,
			GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ, 'final image readback');
		const convergenceReadback = runtime.ensureBuffer(record, 'convergenceReadback', 4,
			GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ, 'convergence readback');

		record.device.queue.writeBuffer(denoiseCamera, 0, HEAPU8.slice(cameraPointer, cameraPointer + 80));
		record.device.queue.writeBuffer(denoisePreviousVP, 0,
			HEAPU8.slice(previousVPPointer, previousVPPointer + 64));
		record.device.queue.writeBuffer(unconvergedCount, 0, new Uint32Array([0]));

		const writeDenoiseParams = (slot, mode, step, seedFlag) => {
			const data = new ArrayBuffer(48);
			const view = new DataView(data);
			view.setUint32(0, width, true);
			view.setUint32(4, height, true);
			view.setFloat32(16, exposure, true);
			view.setUint32(32, mode, true);
			view.setUint32(36, step, true);
			view.setUint32(40, filmic, true);
			view.setUint32(44, seedFlag, true);
			record.device.queue.writeBuffer(denoiseParams, uniformStride * slot, data);
		};

		const parity = record.temporalParity || 0;
		const historyIn = parity === 0 ? historyA : historyB;
		const historyOut = parity === 0 ? historyB : historyA;
		record.temporalParity = parity === 0 ? 1 : 0;

		const encoder = record.device.createCommandEncoder();

		// 1. Accumulate sampleCount samples (primaryMain).
		for (let sampleOffset = 0; sampleOffset < sampleCount; ++sampleOffset) {
			const bindGroup = record.device.createBindGroup({
				layout: record.primaryPipeline.getBindGroupLayout(0),
				entries: [
					{binding: 0, resource: {buffer: record.grids}},
					{binding: 1, resource: {buffer: record.cells}},
					{binding: 2, resource: {buffer: record.materials}},
					{binding: 4, resource: {buffer: record.hits}},
					{binding: 6, resource: {buffer: camera}},
					{binding: 7, resource: {buffer: primaryParams, offset: uniformStride * sampleOffset, size: 32}},
					{binding: 8, resource: {buffer: lighting}},
					{binding: 9, resource: {buffer: record.sampleOutputs}},
					{binding: 10, resource: {buffer: record.emitters}},
					{binding: 11, resource: {buffer: record.ground}},
					{binding: 12, resource: {buffer: record.environmentTexels}},
					{binding: 13, resource: {buffer: record.environmentCdf}},
					{binding: 14, resource: {buffer: record.environmentData}},
					{binding: 15, resource: {buffer: record.mediaData}}
				]
			});
			const pass = encoder.beginComputePass();
			pass.setPipeline(record.primaryPipeline);
			pass.setBindGroup(0, bindGroup);
			pass.dispatchWorkgroups(Math.ceil(pixelCount / 64));
			pass.end();
		}

		// 2. Count still-unconverged pixels (convergenceMain).
		{
			const bindGroup = record.device.createBindGroup({
				layout: record.convergencePipeline.getBindGroupLayout(0),
				entries: [
					{binding: 0, resource: {buffer: record.sampleOutputs}},
					{binding: 1, resource: {buffer: primaryParams, size: 32}},
					{binding: 2, resource: {buffer: unconvergedCount}}
				]
			});
			const pass = encoder.beginComputePass();
			pass.setPipeline(record.convergencePipeline);
			pass.setBindGroup(0, bindGroup);
			pass.dispatchWorkgroups(Math.ceil(pixelCount / 64));
			pass.end();
		}

		// 3. Denoise (denoiseMain: init + 3 a-trous + temporal + tonemap).
		const denoiseBindGroup = (slot, ping, pong) => record.device.createBindGroup({
			layout: record.denoisePipeline.getBindGroupLayout(0),
			entries: [
				{binding: 0, resource: {buffer: record.sampleOutputs}},
				{binding: 1, resource: {buffer: ping}},
				{binding: 2, resource: {buffer: pong}},
				{binding: 3, resource: {buffer: denoiseParams, offset: uniformStride * slot, size: 48}},
				{binding: 4, resource: {buffer: finalImage}},
				{binding: 5, resource: {buffer: denoiseCamera}},
				{binding: 6, resource: {buffer: denoisePreviousVP}},
				{binding: 7, resource: {buffer: historyIn}},
				{binding: 8, resource: {buffer: historyOut}}
			]
		});
		denoisePassPlan.forEach(([mode, step, seedFlag, ping, pong], slot) => {
			writeDenoiseParams(slot, mode, step, seedFlag);
			const pass = encoder.beginComputePass();
			pass.setPipeline(record.denoisePipeline);
			pass.setBindGroup(0, denoiseBindGroup(slot, ping, pong));
			pass.dispatchWorkgroups(Math.ceil(pixelCount / 64));
			pass.end();
		});

		// 4. Read back the packed image and the unconverged count.
		encoder.copyBufferToBuffer(finalImage, 0, finalReadback, 0, imageBytes);
		encoder.copyBufferToBuffer(unconvergedCount, 0, convergenceReadback, 0, 4);
		record.device.queue.submit([encoder.finish()]);
		record.busy = true;
		const generation = record.generation;
		Promise.all([finalReadback.mapAsync(GPUMapMode.READ), convergenceReadback.mapAsync(GPUMapMode.READ)])
			.then(() => {
				if (record.destroyed || generation !== record.generation || record.state !== 2) {
					return;
				}
				record.denoiseResult = new Uint8Array(finalReadback.getMappedRange()).slice();
				record.convergenceResult =
					new Uint32Array(convergenceReadback.getMappedRange().slice(0))[0];
				finalReadback.unmap();
				convergenceReadback.unmap();
				record.resultRayCount = pixelCount;
				record.busy = false;
			}).catch(error => {
				if (record.destroyed || generation !== record.generation) {
					return;
				}
				if (record.state === 1) {
					record.busy = false;
					return;
				}
				runtime.fail(record, 'WebGPU readback failed; continuing on the CPU renderer');
				console.error('Path tracer WebGPU batch readback failed:', error);
			});
		return 1;
	} catch (error) {
		runtime.fail(record, 'WebGPU dispatch was rejected; continuing on the CPU renderer');
		console.error('Path tracer WebGPU batch dispatch failed:', error);
		return 0;
	}
});

EM_JS(int, vengiPathTracerWebGPUTakeBatch,
	  (int handle, void *rgbaPointer, uint32_t capacity, void *unconvergedPointer), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	if (!record || !record.denoiseResult) {
		return 0;
	}
	const byteCount = record.resultRayCount * 4;
	if (capacity < byteCount) {
		return -record.resultRayCount;
	}
	HEAPU8.set(record.denoiseResult, rgbaPointer);
	const countBytes = new Uint8Array(4);
	new DataView(countBytes.buffer).setUint32(0, record.convergenceResult, true);
	HEAPU8.set(countBytes, unconvergedPointer);
	const count = record.resultRayCount;
	record.denoiseResult = null;
	record.convergenceResult = 0;
	record.resultRayCount = 0;
	return count;
});

EM_JS(int, vengiPathTracerWebGPUTakeResults,
	  (int handle, void *hitPointer, uint32_t capacity), {
	const runtime = Module.vengiPathTracerWebGPU;
	const record = runtime ? runtime.backends.get(handle) : null;
	if (!record || !record.result || record.sampleResult) {
		return 0;
	}
	if (capacity < record.resultRayCount) {
		return -record.resultRayCount;
	}
	HEAPU8.set(record.result, hitPointer);
	const count = record.resultRayCount;
	record.result = null;
	record.resultRayCount = 0;
	return count;
});
#endif

namespace voxelpathtracer {

const char *pathTracerWebGPUEventMessage(PathTracerWebGPUEvent event, bool recovered) {
	switch (event) {
	case PathTracerWebGPUEvent::DeviceLost:
		return recovered ? "WebGPU device was lost; recovered and restarted the render"
						 : "WebGPU device was lost; continuing on the CPU renderer";
	case PathTracerWebGPUEvent::Recovered:
		return "WebGPU device was lost; recovered and restarted the render";
	case PathTracerWebGPUEvent::RecoverFailed:
		return "WebGPU device was lost; continuing on the CPU renderer";
	case PathTracerWebGPUEvent::ResourceExhausted:
		return "WebGPU ran out of GPU memory; continuing on the CPU renderer";
	case PathTracerWebGPUEvent::DispatchRejected:
		return "WebGPU dispatch was rejected; continuing on the CPU renderer";
	case PathTracerWebGPUEvent::ReadbackFailed:
		return "WebGPU readback failed; continuing on the CPU renderer";
	case PathTracerWebGPUEvent::InvalidAccumulation:
		return recovered ? "Retrying the first WebGPU batch"
						 : "WebGPU returned invalid accumulation data; continuing on the CPU renderer";
	case PathTracerWebGPUEvent::None:
	default:
		return "";
	}
}

bool pathTracerWebGPUApplyEvent(PathTracerWebGPULifecycle &lifecycle, PathTracerWebGPUEvent event) {
	lifecycle.resetAccumulation = false;
	lifecycle.needsUpload = false;
	switch (event) {
	case PathTracerWebGPUEvent::None:
		return lifecycle.enabled;
	case PathTracerWebGPUEvent::DeviceLost:
		if (lifecycle.enabled && lifecycle.recoveries < lifecycle.maxRecoveries) {
			++lifecycle.recoveries;
			lifecycle.resetAccumulation = true;
			lifecycle.needsUpload = true;
			lifecycle.message = pathTracerWebGPUEventMessage(event, true);
			return true;
		}
		lifecycle.enabled = false;
		lifecycle.message = pathTracerWebGPUEventMessage(event, false);
		return false;
	case PathTracerWebGPUEvent::Recovered:
		if (lifecycle.recoveries == 0u) {
			++lifecycle.recoveries;
		}
		lifecycle.enabled = true;
		lifecycle.resetAccumulation = true;
		lifecycle.needsUpload = true;
		lifecycle.message = pathTracerWebGPUEventMessage(event, true);
		return true;
	case PathTracerWebGPUEvent::RecoverFailed:
		lifecycle.enabled = false;
		lifecycle.message = pathTracerWebGPUEventMessage(event, false);
		return false;
	case PathTracerWebGPUEvent::ResourceExhausted:
	case PathTracerWebGPUEvent::DispatchRejected:
	case PathTracerWebGPUEvent::ReadbackFailed:
		lifecycle.enabled = false;
		lifecycle.message = pathTracerWebGPUEventMessage(event, false);
		return false;
	case PathTracerWebGPUEvent::InvalidAccumulation:
		if (lifecycle.enabled && !lifecycle.invalidRetry) {
			lifecycle.invalidRetry = true;
			lifecycle.resetAccumulation = true;
			lifecycle.message = pathTracerWebGPUEventMessage(event, true);
			return true;
		}
		lifecycle.enabled = false;
		lifecycle.message = pathTracerWebGPUEventMessage(event, false);
		return false;
	}
	return lifecycle.enabled;
}

PathTracerWebGPU::~PathTracerWebGPU() {
	shutdown();
}

bool PathTracerWebGPU::init() {
	if (_state == PathTracerWebGPUState::Ready || _state == PathTracerWebGPUState::Initializing) {
		return true;
	}
#ifdef __EMSCRIPTEN__
	if (_handle >= 0) {
		vengiPathTracerWebGPUDestroy(_handle);
		_handle = -1;
	}
	const char *source = pathTracerTraversalWGSL();
	_handle = vengiPathTracerWebGPUCreate(source, static_cast<uint32_t>(strlen(source)));
	_state = _handle >= 0 ? PathTracerWebGPUState::Initializing : PathTracerWebGPUState::Unavailable;
	_needsUpload = false;
	_fallbackMessage = "";
	return _handle >= 0;
#else
	return false;
#endif
}

void PathTracerWebGPU::abort() {
#ifdef __EMSCRIPTEN__
	if (_handle >= 0) {
		vengiPathTracerWebGPUAbort(_handle);
	}
#endif
	_gridCount = 0u;
	_emitterCount = 0u;
}

void PathTracerWebGPU::shutdown() {
#ifdef __EMSCRIPTEN__
	if (_handle >= 0) {
		vengiPathTracerWebGPUDestroy(_handle);
	}
#endif
	_handle = -1;
	_gridCount = 0u;
	_emitterCount = 0u;
	_needsUpload = false;
	_state = PathTracerWebGPUState::Unavailable;
}

void PathTracerWebGPU::update() {
#ifdef __EMSCRIPTEN__
	if (_handle < 0) {
		return;
	}
	const int stateValue = vengiPathTracerWebGPUState(_handle);
	if (stateValue >= static_cast<int>(PathTracerWebGPUState::Unavailable) &&
		stateValue <= static_cast<int>(PathTracerWebGPUState::Failed)) {
		_state = static_cast<PathTracerWebGPUState>(stateValue);
	}
	_needsUpload = vengiPathTracerWebGPUConsumeNeedsUpload(_handle) != 0;
	char message[256];
	vengiPathTracerWebGPUCopyMessage(_handle, message, sizeof(message));
	_fallbackMessage = message;
#endif
}

bool PathTracerWebGPU::uploadScene(const PathTracerScene &scene) {
	update();
	if (!ready() || scene.grids.empty() || scene.cells.empty() || scene.materials.empty()) {
		return false;
	}
#ifdef __EMSCRIPTEN__
	const size_t gridBytes = scene.grids.size() * sizeof(PathTracerGrid);
	const size_t cellBytes = scene.cells.size() * sizeof(uint32_t);
	const size_t materialBytes = scene.materials.size() * sizeof(PathTracerMaterial);
	const size_t emitterBytes = scene.emitters.size() * sizeof(PathTracerEmitter);
	if (gridBytes > 0xffffffffu || cellBytes > 0xffffffffu || materialBytes > 0xffffffffu ||
		emitterBytes > 0xffffffffu) {
		return false;
	}
	PathTracerMediaData mediaData;
	for (const PathTracerMaterial &material : scene.materials) {
		if (material.surfaceType() == PathTracerSurfaceMedia) {
			mediaData.flags.x = 1u;
			break;
		}
	}
	if (vengiPathTracerWebGPUUploadScene(_handle, scene.grids.data(), static_cast<uint32_t>(gridBytes),
			static_cast<uint32_t>(scene.grids.size()), scene.cells.data(), static_cast<uint32_t>(cellBytes),
			scene.materials.data(), static_cast<uint32_t>(materialBytes),
			scene.emitters.empty() ? nullptr : scene.emitters.data(), static_cast<uint32_t>(emitterBytes),
			static_cast<uint32_t>(scene.emitters.size()), &scene.ground, &mediaData) == 0) {
		return false;
	}
	_gridCount = static_cast<uint32_t>(scene.grids.size());
	_emitterCount = static_cast<uint32_t>(scene.emitters.size());
	return true;
#else
	return false;
#endif
}

bool PathTracerWebGPU::uploadEnvironment(const float *rgba, const float *cdf, uint32_t width, uint32_t height,
										 float cdfSum) {
	update();
	if (!ready() || rgba == nullptr || cdf == nullptr || width == 0u || height == 0u || cdfSum <= 0.0f ||
		width > 0xffffffffu / height) {
		return false;
	}
#ifdef __EMSCRIPTEN__
	return vengiPathTracerWebGPUUploadEnvironment(_handle, rgba, cdf, width, height, cdfSum) != 0;
#else
	return false;
#endif
}

bool PathTracerWebGPU::dispatch(const PathTracerRay *rays, uint32_t rayCount) {
	update();
	if (!ready() || _gridCount == 0u || rays == nullptr || rayCount == 0u) {
		return false;
	}
#ifdef __EMSCRIPTEN__
	return vengiPathTracerWebGPUDispatch(_handle, rays, rayCount) != 0;
#else
	return false;
#endif
}

bool PathTracerWebGPU::dispatchPrimary(const PathTracerCameraData &camera, const PathTracerPrimaryParams &params,
									   const PathTracerLightingData &lighting, uint32_t sampleCount, bool readHits) {
	update();
	if (!ready() || _gridCount == 0u || params.pixelCount == 0u || sampleCount == 0u) {
		return false;
	}
#ifdef __EMSCRIPTEN__
	PathTracerPrimaryParams dispatchParams = params;
	dispatchParams.gridCount = _gridCount;
	dispatchParams.emitterCount = _emitterCount;
	return vengiPathTracerWebGPUDispatchPrimary(_handle, &camera, &dispatchParams, &lighting,
											 dispatchParams.pixelCount, sampleCount, readHits ? 1 : 0) != 0;
#else
	return false;
#endif
}

bool PathTracerWebGPU::busy() const {
#ifdef __EMSCRIPTEN__
	return _handle >= 0 && vengiPathTracerWebGPUBusy(_handle) != 0;
#else
	return false;
#endif
}

bool PathTracerWebGPU::takeResults(PathTracerVoxelHit *hits, uint32_t capacity, uint32_t &hitCount) {
	hitCount = 0u;
	if (!ready() || hits == nullptr || capacity == 0u) {
		return false;
	}
#ifdef __EMSCRIPTEN__
	const int result = vengiPathTracerWebGPUTakeResults(_handle, hits, capacity);
	if (result <= 0) {
		return false;
	}
	hitCount = static_cast<uint32_t>(result);
	return true;
#else
	return false;
#endif
}

bool PathTracerWebGPU::takePrimaryResults(PathTracerVoxelHit *hits, PathTracerSampleOutput *outputs,
									  uint32_t capacity, uint32_t &pixelCount) {
	pixelCount = 0u;
	if (!ready() || hits == nullptr || outputs == nullptr || capacity == 0u) {
		return false;
	}
#ifdef __EMSCRIPTEN__
	const int result = vengiPathTracerWebGPUTakePrimaryResults(_handle, hits, outputs, capacity);
	if (result <= 0) {
		return false;
	}
	pixelCount = static_cast<uint32_t>(result);
	return true;
#else
	return false;
#endif
}

bool PathTracerWebGPU::takePrimaryOutputs(PathTracerSampleOutput *outputs, uint32_t capacity,
										  uint32_t &pixelCount) {
	pixelCount = 0u;
	if (!ready() || outputs == nullptr || capacity == 0u) {
		return false;
	}
#ifdef __EMSCRIPTEN__
	const int result = vengiPathTracerWebGPUTakePrimaryOutputs(_handle, outputs, capacity);
	if (result <= 0) {
		return false;
	}
	pixelCount = static_cast<uint32_t>(result);
	return true;
#else
	return false;
#endif
}

bool PathTracerWebGPU::renderBatch(const PathTracerCameraData &camera, const PathTracerPrimaryParams &params,
								   const PathTracerLightingData &lighting, uint32_t sampleCount, uint32_t width,
								   uint32_t height, float exposure, bool filmic, bool seed) {
	update();
	if (!ready() || _gridCount == 0u || params.pixelCount == 0u || sampleCount == 0u || width == 0u ||
		height == 0u) {
		return false;
	}
#ifdef __EMSCRIPTEN__
	PathTracerPrimaryParams dispatchParams = params;
	dispatchParams.gridCount = _gridCount;
	dispatchParams.emitterCount = _emitterCount;
	const glm::mat4 previousVP = glm::inverse(camera.inverseViewProjection);
	return vengiPathTracerWebGPURenderBatch(_handle, &camera, &dispatchParams, &lighting, sampleCount, width,
											height, exposure, filmic ? 1 : 0, seed ? 1 : 0, &previousVP) != 0;
#else
	return false;
#endif
}

bool PathTracerWebGPU::takeBatch(uint8_t *rgba, uint32_t capacity, uint32_t &pixelCount, uint32_t &unconverged) {
	pixelCount = 0u;
	unconverged = 0u;
	if (!ready() || rgba == nullptr || capacity == 0u) {
		return false;
	}
#ifdef __EMSCRIPTEN__
	const int result = vengiPathTracerWebGPUTakeBatch(_handle, rgba, capacity, &unconverged);
	if (result <= 0) {
		return false;
	}
	pixelCount = static_cast<uint32_t>(result);
	return true;
#else
	return false;
#endif
}

} // namespace voxelpathtracer
