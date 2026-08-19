/**
 * @file
 */

#pragma once

namespace voxelpathtracer {

/**
 * WGSL reference implementation of the production flat-buffer DDA traversal.
 * Kept as a compiled asset until a WebGPU shader asset pipeline exists.
 */
inline const char *pathTracerTraversalWGSL() {
	return R"WGSL(
struct PathTracerGrid {
    minsData: vec4<i32>,
    sizeData: vec4<u32>,
    offsets: vec4<u32>,
    pivotData: vec4<f32>,
    worldMat: mat4x4<f32>,
    invWorldMat: mat4x4<f32>,
};

struct PathTracerMaterial {
    albedoOpacity: vec4<f32>,
    emissionIor: vec4<f32>,
    volumeEmissionAttenuation: vec4<f32>,
    surface: vec4<f32>,
    volume: vec4<f32>,
    flags: vec4<u32>,
};

struct PathTracerRay {
    originMin: vec4<f32>,
    directionMax: vec4<f32>,
    skipCellGrid: vec4<i32>,
    flags: vec4<u32>,
};

struct PathTracerVoxelHit {
    positionT: vec4<f32>,
    normal: vec4<f32>,
    localPosition: vec4<f32>,
    localNormal: vec4<f32>,
    cellGrid: vec4<i32>,
    data: vec4<u32>,
};

struct PathTracerDispatchParams {
    rayCount: u32,
    gridCount: u32,
    reserved0: u32,
    reserved1: u32,
};

struct PathTracerCameraData {
    inverseViewProjection: mat4x4<f32>,
    viewport: vec4<f32>,
};

struct PathTracerPrimaryParams {
    pixelCount: u32,
    sampleIndex: u32,
    gridCount: u32,
    emitterCount: u32,
    adaptiveEnabled: u32,
    adaptiveError: f32,
    adaptiveMinSamples: u32,
    reserved: u32,
};

struct PathTracerLightingData {
    environmentColor: vec4<f32>,
    sunDirectionIntensity: vec4<f32>,
    environmentParams: vec4<f32>,
    flags: vec4<u32>,
};

struct PathTracerSampleOutput {
    radianceAlpha: vec4<f32>,
    albedoFeature: vec4<f32>,
    normalDepth: vec4<f32>,
    positionOpacity: vec4<f32>,
    ids: vec4<u32>,
    moments: vec4<f32>,
};

struct PathTracerEmitter {
    originArea: vec4<f32>,
    edgeU: vec4<f32>,
    edgeV: vec4<f32>,
    normalData: vec4<f32>,
    emissionData: vec4<f32>,
    cellGrid: vec4<i32>,
};

struct PathTracerEmitterSample {
    directionPdf: vec4<f32>,
    radianceDistance: vec4<f32>,
};

struct PathTracerGround {
    boundsMin: vec4<f32>,
    boundsMax: vec4<f32>,
    albedoOpacity: vec4<f32>,
};

struct PathTracerEnvironmentData {
    dimensions: vec4<u32>,
    distribution: vec4<f32>,
};

struct PathTracerEnvironmentSample {
    directionPdf: vec4<f32>,
    radiance: vec4<f32>,
};

struct PathTracerMediaData {
    flags: vec4<u32>,
    params: vec4<f32>,
};

struct PathTracerMediaSample {
    albedoDensity: vec4<f32>,
    emissionScatter: vec4<f32>,
    rimLightData: vec4<f32>,
    cellGrid: vec4<i32>,
    data: vec4<u32>,
};

struct PathTracerMediaIntegration {
    radianceExtinction: vec4<f32>,
    transmissionDepth: vec4<f32>,
    albedoValid: vec4<f32>,
    position: vec4<f32>,
    cellGrid: vec4<i32>,
    data: vec4<u32>,
};

@group(0) @binding(0) var<storage, read> grids: array<PathTracerGrid>;
@group(0) @binding(1) var<storage, read> cells: array<u32>;
@group(0) @binding(2) var<storage, read> materials: array<PathTracerMaterial>;
@group(0) @binding(3) var<storage, read> rays: array<PathTracerRay>;
@group(0) @binding(4) var<storage, read_write> hits: array<PathTracerVoxelHit>;
@group(0) @binding(5) var<uniform> dispatchParams: PathTracerDispatchParams;
@group(0) @binding(6) var<uniform> cameraData: PathTracerCameraData;
@group(0) @binding(7) var<uniform> primaryParams: PathTracerPrimaryParams;
@group(0) @binding(8) var<uniform> lightingData: PathTracerLightingData;
@group(0) @binding(9) var<storage, read_write> sampleOutputs: array<PathTracerSampleOutput>;
@group(0) @binding(10) var<storage, read> emitters: array<PathTracerEmitter>;
@group(0) @binding(11) var<uniform> groundData: PathTracerGround;
@group(0) @binding(12) var<storage, read> environmentTexels: array<vec4<f32>>;
@group(0) @binding(13) var<storage, read> environmentCdf: array<f32>;
@group(0) @binding(14) var<uniform> environmentData: PathTracerEnvironmentData;
@group(0) @binding(15) var<uniform> mediaData: PathTracerMediaData;

const surfaceOpaque: u32 = 0u;
const surfaceAlpha: u32 = 1u;
const surfaceGlass: u32 = 2u;
const surfaceMetal: u32 = 3u;
const surfaceMedia: u32 = 4u;

fn hash32(input: u32) -> u32 {
    var value = (input ^ 61u) ^ (input >> 16u);
    value *= 9u;
    value ^= value >> 4u;
    value *= 0x27d4eb2du;
    value ^= value >> 15u;
    return value;
}

const SOBOL_DIM1_DIRECTIONS: array<u32, 32> = array<u32, 32>(
    1u, 3u, 3u, 9u, 29u, 23u, 71u, 197u, 209u, 627u, 1907u, 1369u,
    4109u, 12327u, 12407u, 36949u, 119041u, 94979u, 291587u, 809225u,
    855325u, 2565911u, 7829319u, 5592517u, 16777681u, 50332019u,
    50332787u, 150998105u, 486542605u, 385885991u, 1191212919u, 3305133397u
);

fn sobolOwenHash(x_in: u32, seed: u32) -> u32 {
    var x = x_in;
    x ^= x * 0x3d20adeau;
    x += seed;
    x *= (seed >> 16u) | 1u;
    x ^= x * 0x05526c56u;
    x ^= x * 0x53a22864u;
    return x;
}

fn sobolRaw(index: u32, dim: u32) -> u32 {
    if (dim == 0u) {
        return reverseBits(index);
    }
    var v = 0u;
    var n = index;
    while (n != 0u) {
        let k = countTrailingZeros(n);
        v ^= SOBOL_DIM1_DIRECTIONS[k] << (31u - k);
        n = n & (n - 1u);
    }
    return v;
}

fn sobolSample(index: u32, dim: u32, seed: u32) -> f32 {
    return f32(sobolOwenHash(sobolRaw(index, dim), seed) >> 8u) * (1.0 / 16777216.0);
}

fn sobol1D(index: u32, scramble: u32) -> f32 {
    return sobolSample(index, 0u, hash32(scramble));
}

fn sobol2D(index: u32, scramble: u32) -> vec2<f32> {
    let x = sobolSample(index, 0u, hash32(scramble ^ 0x68bc21ebu));
    let y = sobolSample(index, 1u, hash32(scramble ^ 0x02e5be93u));
    return vec2<f32>(x, y);
}

fn primaryRay(pixelIndex: u32) -> PathTracerRay {
    let width = u32(cameraData.viewport.x);
    let pixel = vec2<u32>(pixelIndex % width, pixelIndex / width);
    let pixelScramble = hash32(pixelIndex ^ 0xa511e9b3u);
    let cameraSample = sobol2D(primaryParams.sampleIndex, pixelScramble);
    let pixelPosition = vec2<f32>(pixel) + cameraSample;
    let ndc = vec2<f32>(pixelPosition.x * cameraData.viewport.z * 2.0 - 1.0,
                        1.0 - pixelPosition.y * cameraData.viewport.w * 2.0);
    var nearPoint = cameraData.inverseViewProjection * vec4<f32>(ndc, -1.0, 1.0);
    var farPoint = cameraData.inverseViewProjection * vec4<f32>(ndc, 1.0, 1.0);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    var ray: PathTracerRay;
    ray.originMin = vec4<f32>(nearPoint.xyz, 1.0e-4);
    ray.directionMax = vec4<f32>(normalize(farPoint.xyz - nearPoint.xyz), 1.0e30);
    ray.skipCellGrid = vec4<i32>(0, 0, 0, -1);
    ray.flags = vec4<u32>(0u);
    return ray;
}

fn emptyHit(maximumDistance: f32) -> PathTracerVoxelHit {
    var hit: PathTracerVoxelHit;
    hit.positionT = vec4<f32>(0.0, 0.0, 0.0, maximumDistance);
    hit.normal = vec4<f32>(0.0);
    hit.localPosition = vec4<f32>(0.0);
    hit.localNormal = vec4<f32>(0.0);
    hit.cellGrid = vec4<i32>(0, 0, 0, -1);
    hit.data = vec4<u32>(0u);
    return hit;
}

fn gridMins(grid: PathTracerGrid) -> vec3<i32> {
    return grid.minsData.xyz;
}

fn gridSize(grid: PathTracerGrid) -> vec3<i32> {
    return vec3<i32>(grid.sizeData.xyz);
}

fn gridContains(grid: PathTracerGrid, cell: vec3<i32>) -> bool {
    let lower = gridMins(grid);
    let upper = lower + gridSize(grid);
    return all(cell >= lower) && all(cell < upper);
}

fn localCellIndex(grid: PathTracerGrid, cell: vec3<i32>) -> u32 {
    let local = vec3<u32>(cell - gridMins(grid));
    return (local.z * grid.sizeData.y + local.y) * grid.sizeData.x + local.x;
}

fn surfaceIsSolid(surfaceType: u32) -> bool {
    return surfaceType == surfaceOpaque || surfaceType == surfaceMetal;
}

fn traceGrid(ray: PathTracerRay, gridIndex: u32, maximumDistance: f32) -> PathTracerVoxelHit {
    var miss = emptyHit(maximumDistance);
    let grid = grids[gridIndex];
    let directionLength2 = dot(ray.directionMax.xyz, ray.directionMax.xyz);
    if (directionLength2 < 1.0e-16) {
        return miss;
    }
    let worldDirection = ray.directionMax.xyz * inverseSqrt(directionLength2);
    let localOrigin = (grid.invWorldMat * vec4<f32>(ray.originMin.xyz, 1.0)).xyz + grid.pivotData.xyz;
    var localDirection = mat3x3<f32>(grid.invWorldMat[0].xyz, grid.invWorldMat[1].xyz,
                                     grid.invWorldMat[2].xyz) * worldDirection;
    let localLength2 = dot(localDirection, localDirection);
    if (localLength2 < 1.0e-16) {
        return miss;
    }
    localDirection *= inverseSqrt(localLength2);

    let lower = vec3<f32>(gridMins(grid));
    let upper = lower + vec3<f32>(gridSize(grid));
    var t0 = 0.0;
    var t1 = 1.0e30;
    var entryAxis = 0i;
    for (var axis = 0i; axis < 3i; axis += 1i) {
        if (abs(localDirection[axis]) < 1.0e-8) {
            if (localOrigin[axis] < lower[axis] || localOrigin[axis] >= upper[axis]) {
                return miss;
            }
        } else {
            let inverseDirection = 1.0 / localDirection[axis];
            var nearDistance = (lower[axis] - localOrigin[axis]) * inverseDirection;
            var farDistance = (upper[axis] - localOrigin[axis]) * inverseDirection;
            if (nearDistance > farDistance) {
                let swap = nearDistance;
                nearDistance = farDistance;
                farDistance = swap;
            }
            if (nearDistance > t0) {
                t0 = nearDistance;
                entryAxis = axis;
            }
            t1 = min(t1, farDistance);
            if (t0 > t1) {
                return miss;
            }
        }
    }
    if (t1 < 0.0) {
        return miss;
    }
    let startedInside = t0 <= 0.0;
    if (startedInside) {
        t0 = 0.0;
    }

    let samplePosition = localOrigin + localDirection * (t0 + 1.0e-4);
    let minimumCell = gridMins(grid);
    let maximumCell = minimumCell + gridSize(grid) - vec3<i32>(1);
    var cell = clamp(vec3<i32>(floor(samplePosition)), minimumCell, maximumCell);
    let cellStep = vec3<i32>(select(-1i, 1i, localDirection.x >= 0.0),
                             select(-1i, 1i, localDirection.y >= 0.0),
                             select(-1i, 1i, localDirection.z >= 0.0));
    let absoluteDirection = abs(localDirection);
    let delta = vec3<f32>(select(1.0 / max(absoluteDirection.x, 1.0e-8), 1.0e30,
                                        absoluteDirection.x < 1.0e-8),
                          select(1.0 / max(absoluteDirection.y, 1.0e-8), 1.0e30,
                                        absoluteDirection.y < 1.0e-8),
                          select(1.0 / max(absoluteDirection.z, 1.0e-8), 1.0e30,
                                        absoluteDirection.z < 1.0e-8));
    var next = vec3<f32>(1.0e30);
    if (absoluteDirection.x >= 1.0e-8) {
        next.x = select(samplePosition.x - f32(cell.x), f32(cell.x + 1i) - samplePosition.x,
                        localDirection.x >= 0.0) * delta.x;
    }
    if (absoluteDirection.y >= 1.0e-8) {
        next.y = select(samplePosition.y - f32(cell.y), f32(cell.y + 1i) - samplePosition.y,
                        localDirection.y >= 0.0) * delta.y;
    }
    if (absoluteDirection.z >= 1.0e-8) {
        next.z = select(samplePosition.z - f32(cell.z), f32(cell.z + 1i) - samplePosition.z,
                        localDirection.z >= 0.0) * delta.z;
    }

    var lastAxis = entryAxis;
    var enterDistance = t0;
    let maximumSteps = gridSize(grid).x + gridSize(grid).y + gridSize(grid).z + 8i;
    for (var stepIndex = 0i; stepIndex < maximumSteps; stepIndex += 1i) {
        if (!gridContains(grid, cell)) {
            return miss;
        }
        let packed = cells[grid.offsets.x + localCellIndex(grid, cell)];
        if (packed != 0u) {
            let materialIndex = packed - 1u;
            let surfaceType = materials[grid.offsets.y + materialIndex].flags.x;
            let skipOriginCell = i32(gridIndex) == ray.skipCellGrid.w && all(cell == ray.skipCellGrid.xyz);
            let skipSurface = surfaceType == surfaceMedia ||
                              (ray.flags.x != 0u && !surfaceIsSolid(surfaceType));
            if (!skipOriginCell && !skipSurface) {
                var localPosition = localOrigin + localDirection * enterDistance;
                if (!startedInside || stepIndex > 0i) {
                    localPosition[lastAxis] = select(f32(cell[lastAxis] + 1i), f32(cell[lastAxis]),
                                                     cellStep[lastAxis] > 0i);
                }
                let worldPosition = (grid.worldMat *
                    (vec4<f32>(localPosition, 1.0) - vec4<f32>(grid.pivotData.xyz, 0.0))).xyz;
                var worldDistance = dot(worldPosition - ray.originMin.xyz, worldDirection);
                if (worldDistance >= maximumDistance) {
                    return miss;
                }
                worldDistance = max(worldDistance, ray.originMin.w);
                var localNormal = vec3<f32>(0.0);
                localNormal[lastAxis] = select(1.0, -1.0, cellStep[lastAxis] > 0i);
                let inverseLinear = mat3x3<f32>(grid.invWorldMat[0].xyz, grid.invWorldMat[1].xyz,
                                                grid.invWorldMat[2].xyz);
                var worldNormal = transpose(inverseLinear) * localNormal;
                let normalLength2 = dot(worldNormal, worldNormal);
                if (normalLength2 > 1.0e-16) {
                    worldNormal *= inverseSqrt(normalLength2);
                } else {
                    worldNormal = vec3<f32>(0.0, 1.0, 0.0);
                }
                var hit = emptyHit(maximumDistance);
                hit.positionT = vec4<f32>(worldPosition, worldDistance);
                hit.normal = vec4<f32>(worldNormal, 0.0);
                hit.localPosition = vec4<f32>(localPosition, 0.0);
                hit.localNormal = vec4<f32>(localNormal, 0.0);
                hit.cellGrid = vec4<i32>(cell, i32(gridIndex));
                hit.data = vec4<u32>(materialIndex, 1u, 0u, 0u);
                return hit;
            }
        }

        if (next.x < next.y) {
            if (next.x < next.z) {
                enterDistance = t0 + next.x;
                cell.x += cellStep.x;
                lastAxis = 0i;
                next.x += delta.x;
            } else {
                enterDistance = t0 + next.z;
                cell.z += cellStep.z;
                lastAxis = 2i;
                next.z += delta.z;
            }
        } else if (next.y < next.z) {
            enterDistance = t0 + next.y;
            cell.y += cellStep.y;
            lastAxis = 1i;
            next.y += delta.y;
        } else {
            enterDistance = t0 + next.z;
            cell.z += cellStep.z;
            lastAxis = 2i;
            next.z += delta.z;
        }
    }
    return miss;
}

)WGSL"
R"WGSL(fn traceScene(ray: PathTracerRay, gridCount: u32) -> PathTracerVoxelHit {
    var closest = emptyHit(ray.directionMax.w);
    for (var gridIndex = 0u; gridIndex < gridCount; gridIndex += 1u) {
        let candidate = traceGrid(ray, gridIndex, closest.positionT.w);
        if (candidate.data.y != 0u && candidate.positionT.w < closest.positionT.w) {
            closest = candidate;
        }
    }
    return closest;
}

fn traceGround(ray: PathTracerRay, maximumDistance: f32) -> PathTracerVoxelHit {
    var miss = emptyHit(maximumDistance);
    if (groundData.boundsMin.w <= 0.5 || abs(ray.directionMax.y) <= 1.0e-8) {
        return miss;
    }
    let distance = (groundData.boundsMin.y - ray.originMin.y) / ray.directionMax.y;
    if (distance <= ray.originMin.w || distance >= maximumDistance) {
        return miss;
    }
    let position = ray.originMin.xyz + ray.directionMax.xyz * distance;
    if (position.x < groundData.boundsMin.x || position.x > groundData.boundsMax.x ||
        position.z < groundData.boundsMin.z || position.z > groundData.boundsMax.z) {
        return miss;
    }
    var hit = emptyHit(maximumDistance);
    hit.positionT = vec4<f32>(position, distance);
    hit.normal = vec4<f32>(0.0, select(-1.0, 1.0, ray.directionMax.y < 0.0), 0.0, 0.0);
    hit.localPosition = vec4<f32>(position, 0.0);
    hit.localNormal = hit.normal;
    hit.cellGrid = vec4<i32>(0, 0, 0, -1);
    hit.data = vec4<u32>(0u, 1u, 1u, 0u);
    return hit;
}

fn traceTransportScene(ray: PathTracerRay, gridCount: u32) -> PathTracerVoxelHit {
    var closest = traceScene(ray, gridCount);
    let groundHit = traceGround(ray, closest.positionT.w);
    if (groundHit.data.y != 0u && groundHit.positionT.w < closest.positionT.w) {
        closest = groundHit;
    }
    return closest;
}

fn materialForHit(hit: PathTracerVoxelHit) -> PathTracerMaterial {
    if (hit.data.z != 0u) {
        var material: PathTracerMaterial;
        material.albedoOpacity = groundData.albedoOpacity;
        material.emissionIor = vec4<f32>(0.0, 0.0, 0.0, 1.0);
        material.volumeEmissionAttenuation = vec4<f32>(0.0);
        material.surface = vec4<f32>(0.0, 0.8, 0.5, 0.0);
        material.volume = vec4<f32>(0.0);
        material.flags = vec4<u32>(surfaceOpaque, 0u, 0u, 0u);
        return material;
    }
    let grid = grids[u32(hit.cellGrid.w)];
    return materials[grid.offsets.y + hit.data.x];
}

fn tangentFrame(normal: vec3<f32>) -> mat3x3<f32> {
    let axis = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0),
                      abs(normal.x) > 0.1);
    let tangent = normalize(cross(axis, normal));
    let bitangent = cross(normal, tangent);
    return mat3x3<f32>(tangent, bitangent, normal);
}

fn cosineHemisphere(normal: vec3<f32>, sequence: vec2<f32>) -> vec3<f32> {
    let radius = sqrt(sequence.x);
    let phi = 6.283185307179586 * sequence.y;
    let localDirection = vec3<f32>(radius * cos(phi), radius * sin(phi),
                                   sqrt(max(0.0, 1.0 - sequence.x)));
    return normalize(tangentFrame(normal) * localDirection);
}

fn rotateYaw(direction: vec3<f32>, yaw: f32) -> vec3<f32> {
    let cosine = cos(yaw);
    let sine = sin(yaw);
    return vec3<f32>(cosine * direction.x + sine * direction.z, direction.y,
                     -sine * direction.x + cosine * direction.z);
}

fn latLongDirection(u: f32, v: f32) -> vec3<f32> {
    let theta = v * 3.141592653589793;
    let phi = u * 6.283185307179586;
    let sineTheta = sin(theta);
    return vec3<f32>(cos(phi) * sineTheta, cos(theta), sin(phi) * sineTheta);
}

fn environmentTexel(x: i32, y: i32) -> vec3<f32> {
    let width = i32(environmentData.dimensions.x);
    let height = i32(environmentData.dimensions.y);
    let wrappedX = ((x % width) + width) % width;
    let clampedY = clamp(y, 0, height - 1);
    return environmentTexels[u32(clampedY * width + wrappedX)].xyz;
}

fn evalHdri(direction: vec3<f32>) -> vec3<f32> {
    let mapDirection = rotateYaw(normalize(direction), -lightingData.environmentParams.y);
    var u = atan2(mapDirection.z, mapDirection.x) * 0.15915494309189535;
    if (u < 0.0) {
        u += 1.0;
    }
    let v = acos(clamp(mapDirection.y, -1.0, 1.0)) * 0.3183098861837907;
    let width = f32(environmentData.dimensions.x);
    let height = f32(environmentData.dimensions.y);
    let fx = u * width - 0.5;
    let fy = v * height - 0.5;
    let x0 = i32(floor(fx));
    let y0 = i32(floor(fy));
    let fraction = fract(vec2<f32>(fx, fy));
    let row0 = mix(environmentTexel(x0, y0), environmentTexel(x0 + 1, y0), fraction.x);
    let row1 = mix(environmentTexel(x0, y0 + 1), environmentTexel(x0 + 1, y0 + 1), fraction.x);
    return mix(row0, row1, fraction.y) * lightingData.environmentParams.x;
}

fn environmentPdf(direction: vec3<f32>) -> f32 {
    if (lightingData.flags.x != 2u || environmentData.dimensions.w == 0u ||
        environmentData.distribution.x <= 0.0) {
        return 0.07957747154594767;
    }
    let mapDirection = rotateYaw(normalize(direction), -lightingData.environmentParams.y);
    var u = atan2(mapDirection.z, mapDirection.x) * 0.15915494309189535;
    if (u < 0.0) {
        u += 1.0;
    }
    let v = acos(clamp(mapDirection.y, -1.0, 1.0)) * 0.3183098861837907;
    let width = environmentData.dimensions.x;
    let height = environmentData.dimensions.y;
    let x = min(u32(u * f32(width)), width - 1u);
    let y = min(u32(v * f32(height)), height - 1u);
    let index = y * width + x;
	var previous = 0.0;
	if (index > 0u) {
		previous = environmentCdf[index - 1u];
	}
    let weight = environmentCdf[index] - previous;
    let sineTheta = sin(3.141592653589793 * (f32(y) + 0.5) / f32(height));
    let solidAngle = (6.283185307179586 / f32(width)) *
                     (3.141592653589793 / f32(height)) * max(sineTheta, 1.0e-8);
    return (weight / environmentData.distribution.x) / solidAngle;
}

fn sampleEnvironment(normal: vec3<f32>, sequenceIndex: u32,
                     scramble: u32) -> PathTracerEnvironmentSample {
    var sample: PathTracerEnvironmentSample;
    let sequence = sobol2D(sequenceIndex, scramble);
    if (lightingData.flags.x != 2u || environmentData.dimensions.w == 0u ||
        environmentData.distribution.x <= 0.0) {
        let direction = cosineHemisphere(normal, sequence);
        sample.directionPdf = vec4<f32>(direction,
            max(dot(normal, direction), 0.0) * 0.3183098861837907);
        sample.radiance = vec4<f32>(evalEnvironment(direction), 0.0);
        return sample;
    }
	let cdfTarget = sobol1D(sequenceIndex, scramble ^ 0x9e3779b9u) *
                   environmentData.distribution.x;
    var lower = 0u;
    var upper = environmentData.dimensions.z - 1u;
    while (lower < upper) {
        let middle = (lower + upper) / 2u;
		if (environmentCdf[middle] < cdfTarget) {
            lower = middle + 1u;
        } else {
            upper = middle;
        }
    }
    let index = lower;
    let width = environmentData.dimensions.x;
    let height = environmentData.dimensions.y;
    let x = index % width;
    let y = index / width;
    let u = (f32(x) + sequence.x) / f32(width);
    let v = (f32(y) + sequence.y) / f32(height);
    let direction = normalize(rotateYaw(latLongDirection(u, v), lightingData.environmentParams.y));
    var pdf = environmentPdf(direction);
    if (dot(normal, direction) <= 0.0) {
        pdf = 0.0;
    }
    sample.directionPdf = vec4<f32>(direction, pdf);
    sample.radiance = vec4<f32>(environmentTexels[index].xyz * lightingData.environmentParams.x, 0.0);
    return sample;
}

fn evalEnvironment(direction: vec3<f32>) -> vec3<f32> {
    let normalizedDirection = normalize(direction);
	if (lightingData.flags.x == 2u && environmentData.dimensions.w != 0u) {
		return evalHdri(normalizedDirection);
	}
    if (lightingData.flags.x == 1u) {
        let sunDirection = normalize(lightingData.sunDirectionIntensity.xyz);
        let sunAmount = pow(max(0.0, dot(normalizedDirection, sunDirection)), 256.0);
        let zenith = vec3<f32>(0.35, 0.55, 0.95);
        let horizon = vec3<f32>(0.75, 0.82, 0.90);
        let sky = mix(horizon, zenith, clamp(normalizedDirection.y * 0.5 + 0.5, 0.0, 1.0));
        return sky + vec3<f32>(lightingData.sunDirectionIntensity.w * sunAmount);
    }
    let vertical = clamp(0.5 - 0.5 * normalizedDirection.y, 0.0, 1.0);
    return lightingData.environmentColor.xyz * (1.12 - 0.40 * vertical);
}

fn sampleEnvironmentIso(sequenceIndex: u32, scramble: u32) -> PathTracerEnvironmentSample {
    var sample: PathTracerEnvironmentSample;
    let sequence = sobol2D(sequenceIndex, scramble);
    if (lightingData.flags.x != 2u || environmentData.dimensions.w == 0u ||
        environmentData.distribution.x <= 0.0) {
        let y = 1.0 - 2.0 * sequence.x;
        let radius = sqrt(max(0.0, 1.0 - y * y));
        let phi = 6.283185307179586 * sequence.y;
        let direction = vec3<f32>(radius * cos(phi), y, radius * sin(phi));
        sample.directionPdf = vec4<f32>(direction, 0.07957747154594767);
        sample.radiance = vec4<f32>(evalEnvironment(direction), 0.0);
        return sample;
    }
    let cdfTarget = sobol1D(sequenceIndex, scramble ^ 0x9e3779b9u) *
                          environmentData.distribution.x;
    var lower = 0u;
    var upper = environmentData.dimensions.z - 1u;
    while (lower < upper) {
        let middle = (lower + upper) / 2u;
        if (environmentCdf[middle] < cdfTarget) {
            lower = middle + 1u;
        } else {
            upper = middle;
        }
    }
    let index = lower;
    let width = environmentData.dimensions.x;
    let height = environmentData.dimensions.y;
    let x = index % width;
    let y = index / width;
    let u = (f32(x) + sequence.x) / f32(width);
    let v = (f32(y) + sequence.y) / f32(height);
    let direction = normalize(rotateYaw(latLongDirection(u, v), lightingData.environmentParams.y));
    sample.directionPdf = vec4<f32>(direction, environmentPdf(direction));
    sample.radiance = vec4<f32>(environmentTexels[index].xyz * lightingData.environmentParams.x, 0.0);
    return sample;
}

)WGSL"
R"WGSL(fn ggxD(normalHalf: f32, alpha: f32) -> f32 {
    let alpha2 = alpha * alpha;
    let denominator = normalHalf * normalHalf * (alpha2 - 1.0) + 1.0;
    return alpha2 / (3.141592653589793 * denominator * denominator);
}

fn ggxG1(normalDirection: f32, alpha: f32) -> f32 {
    let nd = max(normalDirection, 0.0);
    let alpha2 = alpha * alpha;
    return 2.0 * nd / max(nd + sqrt(alpha2 + (1.0 - alpha2) * nd * nd), 1.0e-8);
}

fn fresnelSchlick3(f0: vec3<f32>, viewHalf: f32) -> vec3<f32> {
    let amount = 1.0 - clamp(viewHalf, 0.0, 1.0);
    let amount2 = amount * amount;
    let amount5 = amount2 * amount2 * amount;
    return f0 + (vec3<f32>(1.0) - f0) * amount5;
}

fn fresnelSchlickIor(cosine: f32, ior: f32) -> f32 {
    var r0 = (1.0 - ior) / (1.0 + ior);
    r0 *= r0;
    let amount = 1.0 - clamp(cosine, 0.0, 1.0);
    let amount2 = amount * amount;
    return r0 + (1.0 - r0) * amount2 * amount2 * amount;
}

fn beerAttenuation(albedo: vec3<f32>, opacity: f32, attenuation: f32,
                   thickness: f32) -> vec3<f32> {
    let clampedOpacity = clamp(opacity, 0.0, 1.0);
    let passAmount = max(1.0 - clampedOpacity, 1.0e-4);
    let sigma = -log(passAmount) + clamp(attenuation, 0.0, 1.0) * 4.0;
    let stain = mix(vec3<f32>(1.0), albedo, clampedOpacity);
    return stain * exp(-sigma * max(thickness, 0.0));
}

fn evalOpaqueFcos(albedo: vec3<f32>, metal: f32, specular: f32, alpha: f32,
                   normal: vec3<f32>, outgoing: vec3<f32>, incoming: vec3<f32>) -> vec3<f32> {
    let normalLight = dot(normal, incoming);
    let normalView = dot(normal, outgoing);
    if (normalLight <= 1.0e-6 || normalView <= 1.0e-6) {
        return vec3<f32>(0.0);
    }
    let halfVector = normalize(outgoing + incoming);
    let normalHalf = max(dot(normal, halfVector), 0.0);
    let viewHalf = max(dot(outgoing, halfVector), 0.0);
    let distribution = ggxD(normalHalf, alpha);
    let geometry = ggxG1(normalView, alpha) * ggxG1(normalLight, alpha);
    let clampedMetal = clamp(metal, 0.0, 1.0);
    let specularAmount = 0.2 + 0.8 * clamp(specular, 0.0, 1.0);
    let f0 = mix(vec3<f32>(0.04 * specularAmount), albedo, clampedMetal);
    let fresnel = fresnelSchlick3(f0, viewHalf);
    let diffuse = albedo * (vec3<f32>(1.0) - fresnel) * (1.0 - clampedMetal) *
                  (0.3183098861837907 * normalLight);
    return diffuse + fresnel * (distribution * geometry / (4.0 * normalView));
}

fn opaqueSpecularProbability(albedo: vec3<f32>, metal: f32, specular: f32,
                             normal: vec3<f32>, outgoing: vec3<f32>) -> f32 {
    let specularAmount = 0.2 + 0.8 * clamp(specular, 0.0, 1.0);
    let f0 = mix(vec3<f32>(0.04 * specularAmount), albedo, clamp(metal, 0.0, 1.0));
    let fresnel = fresnelSchlick3(f0, max(dot(normal, outgoing), 0.0));
    return clamp(0.25 + 0.75 * dot(fresnel, vec3<f32>(0.3333333333333333)), 0.15, 0.95);
}

fn opaquePdf(albedo: vec3<f32>, metal: f32, specular: f32, alpha: f32,
             normal: vec3<f32>, outgoing: vec3<f32>, incoming: vec3<f32>) -> f32 {
    let normalLight = max(dot(normal, incoming), 0.0);
    let normalView = max(dot(normal, outgoing), 0.0);
    if (normalLight <= 1.0e-6 || normalView <= 1.0e-6) {
        return 0.0;
    }
    let specularProbability = opaqueSpecularProbability(albedo, metal, specular, normal, outgoing);
    let halfSum = outgoing + incoming;
    var specularPdf = 0.0;
    if (dot(halfSum, halfSum) > 1.0e-12) {
        let halfVector = normalize(halfSum);
        let normalHalf = max(dot(normal, halfVector), 0.0);
        specularPdf = ggxD(normalHalf, alpha) * ggxG1(normalView, alpha) /
                      max(4.0 * normalView, 1.0e-6);
    }
    return specularProbability * specularPdf +
           (1.0 - specularProbability) * normalLight * 0.3183098861837907;
}

fn sampleGgxVisibleHalf(normal: vec3<f32>, outgoing: vec3<f32>, alpha: f32,
                        sequence: vec2<f32>) -> vec3<f32> {
    let frame = tangentFrame(normal);
    let tangent = frame[0];
    let bitangent = frame[1];
    let view = vec3<f32>(dot(outgoing, tangent), dot(outgoing, bitangent),
                         max(dot(outgoing, normal), 1.0e-6));
    let stretchedView = normalize(vec3<f32>(alpha * view.x, alpha * view.y, view.z));
    let lensq = stretchedView.x * stretchedView.x + stretchedView.y * stretchedView.y;
	var tangent1 = vec3<f32>(1.0, 0.0, 0.0);
	if (lensq > 1.0e-8) {
		tangent1 = vec3<f32>(-stretchedView.y, stretchedView.x, 0.0) / sqrt(lensq);
	}
    let tangent2 = cross(stretchedView, tangent1);
    let radius = sqrt(sequence.x);
    let phi = 6.283185307179586 * sequence.y;
    let diskX = radius * cos(phi);
    var diskY = radius * sin(phi);
    let blend = 0.5 * (1.0 + stretchedView.z);
    diskY = (1.0 - blend) * sqrt(max(0.0, 1.0 - diskX * diskX)) + blend * diskY;
    let diskZ = sqrt(max(0.0, 1.0 - diskX * diskX - diskY * diskY));
    let stretchedNormal = diskX * tangent1 + diskY * tangent2 + diskZ * stretchedView;
    let localHalf = normalize(vec3<f32>(alpha * stretchedNormal.x,
                                        alpha * stretchedNormal.y,
                                        max(stretchedNormal.z, 0.0)));
    return normalize(tangent * localHalf.x + bitangent * localHalf.y + normal * localHalf.z);
}

fn powerHeuristic(pdfA: f32, pdfB: f32) -> f32 {
    let squareA = pdfA * pdfA;
    let squareB = pdfB * pdfB;
    return squareA / max(squareA + squareB, 1.0e-12);
}

fn sampleEmitter(position: vec3<f32>, normal: vec3<f32>, skipCellGrid: vec4<i32>,
                 sequenceIndex: u32, scramble: u32) -> PathTracerEmitterSample {
    var result: PathTracerEmitterSample;
    result.directionPdf = vec4<f32>(0.0);
    result.radianceDistance = vec4<f32>(0.0);
    if (primaryParams.emitterCount == 0u) {
        return result;
    }
    let selection = sobol1D(sequenceIndex, scramble ^ 0x27d4eb2du);
    let emitterIndex = min(u32(selection * f32(primaryParams.emitterCount)),
                           primaryParams.emitterCount - 1u);
    let emitter = emitters[emitterIndex];
    let faceSample = sobol2D(sequenceIndex, scramble ^ 0xb5297a4du);
    let lightPosition = emitter.originArea.xyz + emitter.edgeU.xyz * faceSample.x +
                        emitter.edgeV.xyz * faceSample.y;
    let toLight = lightPosition - position;
    let distanceSquared = dot(toLight, toLight);
    if (distanceSquared <= 1.0e-8 || emitter.originArea.w <= 1.0e-8) {
        return result;
    }
    let distance = sqrt(distanceSquared);
    let direction = toLight / distance;
    let normalLight = dot(normal, direction);
    let lightCosine = dot(emitter.normalData.xyz, -direction);
	let volumePoint = dot(normal, normal) < 1.0e-8;
    if ((!volumePoint && normalLight <= 1.0e-6) || lightCosine <= 1.0e-6) {
        return result;
    }
    var shadowRay: PathTracerRay;
    shadowRay.originMin = vec4<f32>(position, 1.0e-4);
    shadowRay.directionMax = vec4<f32>(direction, max(distance - 2.0e-3, 1.0e-4));
    shadowRay.skipCellGrid = skipCellGrid;
    shadowRay.flags = vec4<u32>(1u, 0u, 0u, 0u);
    if (traceTransportScene(shadowRay, primaryParams.gridCount).data.y != 0u) {
        return result;
    }
    let pdf = distanceSquared /
              (f32(primaryParams.emitterCount) * emitter.originArea.w * lightCosine);
    result.directionPdf = vec4<f32>(direction, pdf);
    result.radianceDistance = vec4<f32>(emitter.emissionData.xyz, distance);
    return result;
}

fn emitterHitPdf(position: vec3<f32>, hit: PathTracerVoxelHit) -> f32 {
    if (primaryParams.emitterCount == 0u || hit.data.y == 0u) {
        return 0.0;
    }
    let toLight = hit.positionT.xyz - position;
    let distanceSquared = dot(toLight, toLight);
    if (distanceSquared <= 1.0e-8) {
        return 0.0;
    }
    let direction = toLight * inverseSqrt(distanceSquared);
    for (var emitterIndex = 0u; emitterIndex < primaryParams.emitterCount; emitterIndex += 1u) {
        let emitter = emitters[emitterIndex];
        if (all(emitter.cellGrid == hit.cellGrid) && emitter.originArea.w > 1.0e-8 &&
            dot(emitter.normalData.xyz, hit.normal.xyz) > 0.999) {
            let lightCosine = dot(emitter.normalData.xyz, -direction);
            if (lightCosine <= 1.0e-6) {
                return 0.0;
            }
            return distanceSquared /
                   (f32(primaryParams.emitterCount) * emitter.originArea.w * lightCosine);
        }
    }
    return 0.0;
}

fn mediaAt(worldPosition: vec3<f32>) -> PathTracerMediaSample {
	var sample: PathTracerMediaSample;
	sample.albedoDensity = vec4<f32>(0.0);
	sample.emissionScatter = vec4<f32>(0.0);
	sample.rimLightData = vec4<f32>(0.0);
	sample.cellGrid = vec4<i32>(0, 0, 0, -1);
	sample.data = vec4<u32>(0u);
	for (var gridIndex = 0u; gridIndex < primaryParams.gridCount; gridIndex += 1u) {
		let grid = grids[gridIndex];
		let localPosition = (grid.invWorldMat * vec4<f32>(worldPosition, 1.0)).xyz + grid.pivotData.xyz;
		let cell = vec3<i32>(floor(localPosition));
		if (!gridContains(grid, cell)) {
			continue;
		}
		let packed = cells[grid.offsets.x + localCellIndex(grid, cell)];
		if (packed == 0u) {
			continue;
		}
		let materialIndex = packed - 1u;
		let material = materials[grid.offsets.y + materialIndex];
		if (material.flags.x != surfaceMedia) {
			continue;
		}
		sample.albedoDensity = vec4<f32>(material.albedoOpacity.xyz,
			clamp(material.surface.w, 0.0, 1.0));
		sample.emissionScatter = vec4<f32>(material.volumeEmissionAttenuation.xyz,
			clamp(material.volume.y, 0.0, 1.0));
		sample.rimLightData.x = clamp(material.volume.x, -0.95, 0.95);
		sample.cellGrid = vec4<i32>(cell, i32(gridIndex));
		sample.data = vec4<u32>(materialIndex, gridIndex, surfaceMedia, 1u);
		return sample;
	}
	return sample;
}

fn volumeSigmaT(density: f32) -> f32 {
	let clampedDensity = clamp(density, 0.0, 1.0);
	return clampedDensity * clampedDensity * 2.0 + clampedDensity * 0.2;
}

fn henyeyGreenstein(rimLight: f32, cosine: f32) -> f32 {
	let clampedPhase = clamp(rimLight, -0.95, 0.95);
	let phase2 = clampedPhase * clampedPhase;
	let denominator = 1.0 + phase2 - 2.0 * clampedPhase * cosine;
	return (1.0 - phase2) /
		(12.566370614359172 * denominator * sqrt(max(denominator, 1.0e-8)));
}

fn integrateMedia(ray: PathTracerRay, maximumDistance: f32, sequenceIndex: u32,
				  scramble: u32) -> PathTracerMediaIntegration {
	var integrated: PathTracerMediaIntegration;
	integrated.radianceExtinction = vec4<f32>(0.0);
	integrated.transmissionDepth = vec4<f32>(1.0, 1.0, 1.0, 0.0);
	integrated.albedoValid = vec4<f32>(0.0);
	integrated.position = vec4<f32>(0.0);
	integrated.cellGrid = vec4<i32>(0, 0, 0, -1);
	integrated.data = vec4<u32>(0u);
	if (mediaData.flags.x == 0u) {
		return integrated;
	}
	let direction = normalize(ray.directionMax.xyz);
	let stepLength = max(mediaData.params.x, 0.05);
	let mediaTravel = max(mediaData.params.y, stepLength);
	let jitter = sobol1D(sequenceIndex, scramble ^ 0xd1b54a35u) * stepLength;
	let environmentSample = sampleEnvironmentIso(sequenceIndex, scramble ^ 0x94d049bbu);
	var transmission = vec3<f32>(1.0);
	var sampled = 0u;
	var firstHit = -1.0;
	for (var probeIndex = 0u; probeIndex < 2048u; probeIndex += 1u) {
		let distance = jitter + f32(probeIndex) * stepLength;
		if (distance >= maximumDistance) {
			break;
		}
		let position = ray.originMin.xyz + direction * distance;
		let medium = mediaAt(position);
		if (medium.data.w == 0u) {
			continue;
		}
		if (firstHit < 0.0) {
			firstHit = distance;
		}
		if (distance - firstHit >= mediaTravel || sampled >= mediaData.flags.y) {
			break;
		}
		sampled += 1u;
		if (integrated.albedoValid.w == 0.0) {
			integrated.albedoValid = vec4<f32>(medium.albedoDensity.xyz, 1.0);
			integrated.transmissionDepth.w = distance;
			integrated.position = vec4<f32>(position, 1.0);
			integrated.cellGrid = medium.cellGrid;
			integrated.data = medium.data;
		}
		let extinction = volumeSigmaT(medium.albedoDensity.w);
		let segmentTransmission = exp(-extinction * stepLength);
		let interaction = 1.0 - segmentTransmission;
		let emissionContribution = transmission * medium.emissionScatter.xyz * interaction;
		integrated.radianceExtinction = vec4<f32>(
			integrated.radianceExtinction.xyz + emissionContribution,
			integrated.radianceExtinction.w);
		if (medium.emissionScatter.w > 1.0e-4 && environmentSample.directionPdf.w > 1.0e-8) {
			let lightDirection = environmentSample.directionPdf.xyz;
			var shadowRay: PathTracerRay;
			shadowRay.originMin = vec4<f32>(position, 1.0e-4);
			shadowRay.directionMax = vec4<f32>(lightDirection, 1.0e30);
			shadowRay.skipCellGrid = vec4<i32>(0, 0, 0, -1);
			shadowRay.flags = vec4<u32>(1u, 0u, 0u, 0u);
			if (traceTransportScene(shadowRay, primaryParams.gridCount).data.y == 0u) {
				let phase = henyeyGreenstein(medium.rimLightData.x, dot(direction, lightDirection));
				let environmentContribution = transmission * medium.albedoDensity.xyz *
					medium.emissionScatter.w * interaction * phase * environmentSample.radiance.xyz /
					environmentSample.directionPdf.w;
				integrated.radianceExtinction = vec4<f32>(
					integrated.radianceExtinction.xyz + environmentContribution,
					integrated.radianceExtinction.w);
			}
			let emitterSample = sampleEmitter(position, vec3<f32>(0.0), vec4<i32>(0, 0, 0, -1),
				sequenceIndex + probeIndex, scramble ^ 0x369dea0fu);
			if (emitterSample.directionPdf.w > 1.0e-8) {
				let phase = henyeyGreenstein(medium.rimLightData.x,
					dot(direction, emitterSample.directionPdf.xyz));
				let emitterContribution = transmission * medium.albedoDensity.xyz *
					medium.emissionScatter.w * interaction * phase *
					emitterSample.radianceDistance.xyz / emitterSample.directionPdf.w;
				integrated.radianceExtinction = vec4<f32>(
					integrated.radianceExtinction.xyz + emitterContribution,
					integrated.radianceExtinction.w);
			}
		}
		transmission *= segmentTransmission;
		if (max(transmission.x, max(transmission.y, transmission.z)) < 1.0e-4) {
			transmission = vec3<f32>(0.0);
			break;
		}
	}
	integrated.transmissionDepth = vec4<f32>(transmission, integrated.transmissionDepth.w);
	integrated.radianceExtinction.w = clamp(1.0 - dot(transmission, vec3<f32>(0.3333333333)), 0.0, 1.0);
	return integrated;
}

fn studioBevel(position: vec3<f32>, normal: vec3<f32>) -> f32 {
    let fractional = fract(position);
    var edge: f32;
    if (abs(normal.y) > 0.5) {
        edge = min(min(fractional.x, 1.0 - fractional.x),
                   min(fractional.z, 1.0 - fractional.z));
    } else if (abs(normal.x) > 0.5) {
        edge = min(min(fractional.y, 1.0 - fractional.y),
                   min(fractional.z, 1.0 - fractional.z));
    } else {
        edge = min(min(fractional.x, 1.0 - fractional.x),
                   min(fractional.y, 1.0 - fractional.y));
    }
    let amount = clamp((edge - 0.008) / (0.028 - 0.008), 0.0, 1.0);
    return mix(0.62, 1.02, amount);
}

)WGSL"
R"WGSL(fn shadePrimary(pixelIndex: u32, initialRay: PathTracerRay,
                primaryHit: PathTracerVoxelHit) -> PathTracerSampleOutput {
    var output: PathTracerSampleOutput;
    output.radianceAlpha = vec4<f32>(0.0);
    output.albedoFeature = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    output.normalDepth = vec4<f32>(0.0);
    output.positionOpacity = vec4<f32>(0.0);
    output.ids = vec4<u32>(0u);
    output.moments = vec4<f32>(0.0, 1.0, 0.0, 0.0);
	if (primaryHit.data.y != 0u) {
		let primaryIsGround = primaryHit.data.z != 0u;
		let primaryGridIndex = select(u32(primaryHit.cellGrid.w), 0xffffffffu, primaryIsGround);
		let primaryMaterial = materialForHit(primaryHit);
		let primarySurfaceType = primaryMaterial.flags.x;
		let primaryNormal = select(-primaryHit.normal.xyz, primaryHit.normal.xyz,
			dot(primaryHit.normal.xyz, initialRay.directionMax.xyz) < 0.0);
		var feature = 1.0;
		if (!primaryIsGround && lightingData.flags.z != 0u &&
			(primarySurfaceType == surfaceOpaque || primarySurfaceType == surfaceMetal)) {
			feature = studioBevel(primaryHit.localPosition.xyz, primaryHit.localNormal.xyz);
		}
		output.albedoFeature = vec4<f32>(primaryMaterial.albedoOpacity.xyz, feature);
		output.normalDepth = vec4<f32>(primaryNormal, primaryHit.positionT.w);
		output.positionOpacity = vec4<f32>(primaryHit.positionT.xyz, primaryMaterial.albedoOpacity.w);
		output.ids = vec4<u32>(primaryHit.data.x, primaryGridIndex, primarySurfaceType, 1u);
	}

    let pixelScramble = hash32(pixelIndex ^ 0xa511e9b3u);
    let maxBounces = clamp(lightingData.flags.w, 1u, 8u);
    var ray = initialRay;
    var hit = primaryHit;
    var throughput = vec3<f32>(1.0);
    var radiance = vec3<f32>(0.0);
    var specularChain = true;
    var emitterMisValid = false;
    var previousBsdfPdf = 0.0;
	var previousEnvironmentPdf = 0.0;
    var previousPosition = vec3<f32>(0.0);
	var pathAlpha = select(1.0, 0.0, primaryHit.data.y == 0u && lightingData.flags.y != 0u);
    for (var bounce = 0u; bounce < maxBounces; bounce += 1u) {
		let sequenceIndex = primaryParams.sampleIndex * maxBounces + bounce;
		let bounceScramble = pixelScramble ^ ((bounce + 1u) * 0x9e3779b9u);
		let segmentDistance = select(hit.positionT.w, 1.0e30, hit.data.y == 0u);
		let media = integrateMedia(ray, segmentDistance, sequenceIndex,
			bounceScramble ^ 0x7f4a7c15u);
		radiance += throughput * media.radianceExtinction.xyz;
		throughput *= media.transmissionDepth.xyz;
		if (bounce == 0u) {
			pathAlpha = max(pathAlpha, media.radianceExtinction.w);
			if (media.albedoValid.w > 0.0 && media.radianceExtinction.w > 0.06) {
				output.albedoFeature = vec4<f32>(media.albedoValid.xyz, 1.0);
				output.normalDepth = vec4<f32>(-normalize(ray.directionMax.xyz),
					media.transmissionDepth.w);
				output.positionOpacity = vec4<f32>(media.position.xyz, media.radianceExtinction.w);
				output.ids = media.data;
			}
		}
        if (hit.data.y == 0u) {
			if ((bounce == 0u && lightingData.flags.y == 0u) ||
				(bounce > 0u && specularChain)) {
				radiance += throughput * evalEnvironment(ray.directionMax.xyz);
			} else if (previousEnvironmentPdf > 0.0) {
				radiance += throughput * evalEnvironment(ray.directionMax.xyz) *
					powerHeuristic(previousBsdfPdf, previousEnvironmentPdf);
			}
			// Miss pixels have no surface. A zero guide normal makes the
			// a-trous pow(n.n, 32) term zero -- including the pixel's own
			// weight -- so denoiseMain writes black over the HDRI.
			if (bounce == 0u && lightingData.flags.y == 0u &&
				dot(output.normalDepth.xyz, output.normalDepth.xyz) <= 1.0e-12) {
				output.albedoFeature = vec4<f32>(evalEnvironment(ray.directionMax.xyz), 1.0);
				output.normalDepth = vec4<f32>(-normalize(ray.directionMax.xyz), 0.0);
			}
            break;
        }

		let isGround = hit.data.z != 0u;
		let material = materialForHit(hit);
        let surfaceType = material.flags.x;
        let facingNormal = select(-hit.normal.xyz, hit.normal.xyz,
                                  dot(hit.normal.xyz, ray.directionMax.xyz) < 0.0);
        var bounceFeature = 1.0;
		if (!isGround && lightingData.flags.z != 0u &&
            (surfaceType == surfaceOpaque || surfaceType == surfaceMetal)) {
            bounceFeature = studioBevel(hit.localPosition.xyz, hit.localNormal.xyz);
        }
        let albedo = material.albedoOpacity.xyz * bounceFeature;
		if (max(material.emissionIor.x, max(material.emissionIor.y, material.emissionIor.z)) > 0.0) {
			var emissionWeight = 1.0;
			if (bounce > 0u && !specularChain && emitterMisValid) {
				let lightPdf = emitterHitPdf(previousPosition, hit);
				if (lightPdf > 0.0) {
					emissionWeight = powerHeuristic(previousBsdfPdf, lightPdf);
				}
			}
			radiance += throughput * material.emissionIor.xyz * emissionWeight;
		}
		if (surfaceType == surfaceGlass) {
			emitterMisValid = false;
			if (bounce + 1u >= maxBounces) {
				break;
			}
			let entering = dot(ray.directionMax.xyz, hit.normal.xyz) < 0.0;
			let interfaceNormal = select(-hit.normal.xyz, hit.normal.xyz, entering);
			let ior = max(material.emissionIor.w, 1.01);
			let eta = select(ior, 1.0 / ior, entering);
			let cosine = clamp(-dot(ray.directionMax.xyz, interfaceNormal), 0.0, 1.0);
			let refracted = refract(ray.directionMax.xyz, interfaceNormal, eta);
			let totalInternalReflection = dot(refracted, refracted) < 1.0e-12;
			let fresnel = select(fresnelSchlickIor(cosine, ior), 1.0,
								 totalInternalReflection);
			let reflectionChoice = sobol1D(sequenceIndex, bounceScramble ^ 0x51ed270bu);
			var nextDirection: vec3<f32>;
			var nextOrigin: vec3<f32>;
			if (totalInternalReflection || reflectionChoice < fresnel) {
				nextDirection = reflect(ray.directionMax.xyz, interfaceNormal);
				nextOrigin = hit.positionT.xyz + interfaceNormal * 1.0e-3;
			} else {
				nextDirection = normalize(refracted);
				nextOrigin = hit.positionT.xyz + nextDirection * 1.0e-3;
				throughput *= beerAttenuation(material.albedoOpacity.xyz,
					material.albedoOpacity.w, material.volumeEmissionAttenuation.w, 1.0);
			}
			var continuationRay: PathTracerRay;
			continuationRay.originMin = vec4<f32>(nextOrigin, 1.0e-4);
			continuationRay.directionMax = vec4<f32>(normalize(nextDirection), 1.0e30);
			continuationRay.skipCellGrid = hit.cellGrid;
			continuationRay.flags = vec4<u32>(0u);
			let continuationHit = traceTransportScene(continuationRay, primaryParams.gridCount);
			ray = continuationRay;
			hit = continuationHit;
			continue;
		}

		if (surfaceType == surfaceAlpha) {
			let alphaChoice = sobol1D(sequenceIndex, bounceScramble ^ 0x85ebca6bu);
			if (alphaChoice >= material.albedoOpacity.w) {
				emitterMisValid = false;
				if (bounce + 1u >= maxBounces) {
					break;
				}
				throughput *= mix(vec3<f32>(1.0), material.albedoOpacity.xyz,
								  material.albedoOpacity.w * 0.35);
				var continuationRay: PathTracerRay;
				continuationRay.originMin = vec4<f32>(hit.positionT.xyz + ray.directionMax.xyz * 1.0e-3, 1.0e-4);
				continuationRay.directionMax = ray.directionMax;
				continuationRay.skipCellGrid = hit.cellGrid;
				continuationRay.flags = vec4<u32>(0u);
				let continuationHit = traceTransportScene(continuationRay, primaryParams.gridCount);
				ray = continuationRay;
				hit = continuationHit;
				continue;
			}
		}

        if (surfaceType != surfaceOpaque && surfaceType != surfaceMetal &&
			surfaceType != surfaceAlpha) {
            break;
        }

)WGSL"
R"WGSL(
		specularChain = false;
        let roughness = clamp(material.surface.y, 0.04, 1.0);
        let alpha = roughness * roughness;
        let outgoing = -ray.directionMax.xyz;
        // Single environment next-event sample with full MIS. The four-sample
        // stratification heuristic is dropped: 1 NEE + 1 BSDF + full MIS.
        let includeEnvironmentMis = bounce + 1u < maxBounces;
        let environmentSample = sampleEnvironment(facingNormal, sequenceIndex,
            bounceScramble ^ 0x68bc21ebu);
        let lightDirection = environmentSample.directionPdf.xyz;
        var shadowRay: PathTracerRay;
        shadowRay.originMin = vec4<f32>(hit.positionT.xyz + facingNormal * 1.0e-3, 1.0e-4);
        shadowRay.directionMax = vec4<f32>(lightDirection, 1.0e30);
        shadowRay.skipCellGrid = hit.cellGrid;
        shadowRay.flags = vec4<u32>(1u, 0u, 0u, 0u);
        let shadowHit = traceTransportScene(shadowRay, primaryParams.gridCount);
        if (shadowHit.data.y == 0u) {
            let lightPdf = environmentSample.directionPdf.w;
            if (lightPdf > 1.0e-8) {
                let fcos = evalOpaqueFcos(albedo, material.surface.x, material.surface.z, alpha,
                                           facingNormal, outgoing, lightDirection);
                let bsdfPdf = opaquePdf(albedo, material.surface.x, material.surface.z, alpha,
                                        facingNormal, outgoing, lightDirection);
                let misWeight = select(1.0, powerHeuristic(lightPdf, bsdfPdf),
                                       includeEnvironmentMis);
                radiance += throughput * fcos * environmentSample.radiance.xyz * misWeight /
                    lightPdf;
            }
        }

		let emitterSample = sampleEmitter(
			hit.positionT.xyz + facingNormal * 1.0e-3, facingNormal, hit.cellGrid,
			sequenceIndex, bounceScramble ^ 0x165667b1u);
		if (emitterSample.directionPdf.w > 1.0e-8) {
			let emitterDirection = emitterSample.directionPdf.xyz;
			let fcos = evalOpaqueFcos(albedo, material.surface.x, material.surface.z, alpha,
								   facingNormal, outgoing, emitterDirection);
			let bsdfLightPdf = opaquePdf(albedo, material.surface.x, material.surface.z, alpha,
									 facingNormal, outgoing, emitterDirection);
			let misWeight = select(1.0,
				powerHeuristic(emitterSample.directionPdf.w, bsdfLightPdf),
				bounce + 1u < maxBounces);
			radiance += throughput * fcos * emitterSample.radianceDistance.xyz * misWeight /
						emitterSample.directionPdf.w;
		}

        if (bounce + 1u >= maxBounces) {
            break;
        }
        let bsdfSequence = sobol2D(sequenceIndex, bounceScramble ^ 0x02e5be93u);
        let specularChoice = sobol1D(sequenceIndex, bounceScramble ^ 0x7f4a7c15u);
        let specularProbability = opaqueSpecularProbability(
            albedo, material.surface.x, material.surface.z, facingNormal, outgoing);
        var incoming: vec3<f32>;
        if (specularChoice < specularProbability) {
            let halfVector = sampleGgxVisibleHalf(facingNormal, outgoing, alpha, bsdfSequence);
            incoming = reflect(-outgoing, halfVector);
        } else {
            incoming = cosineHemisphere(facingNormal, bsdfSequence);
        }
        let bsdfPdf = opaquePdf(albedo, material.surface.x, material.surface.z, alpha,
                                facingNormal, outgoing, incoming);
        if (dot(facingNormal, incoming) <= 1.0e-6 || bsdfPdf <= 1.0e-6) {
            break;
        }
        throughput *= evalOpaqueFcos(albedo, material.surface.x, material.surface.z, alpha,
                                     facingNormal, outgoing, incoming) / bsdfPdf;
        // Unbiased Russian roulette replaces the old hard throughput cutoff,
        // which silently dropped the residual energy of dim paths (darkened
        // deep-indirect corners). Terminate probabilistically and rescale
        // survivors so the expected contribution is unchanged.
        let throughputMax = max(throughput.x, max(throughput.y, throughput.z));
        if (throughputMax < 0.2) {
            let survival = max(throughputMax, 1.0e-4);
            if (sobol1D(sequenceIndex, bounceScramble ^ 0x5be0cd19u) > survival) {
                break;
            }
            throughput /= survival;
        }
		previousBsdfPdf = bsdfPdf;
		previousEnvironmentPdf = max(dot(facingNormal, incoming), 0.0) * 0.3183098861837907;
		if (lightingData.flags.x == 2u && environmentData.dimensions.w != 0u) {
			previousEnvironmentPdf = environmentPdf(incoming);
		}
		previousPosition = hit.positionT.xyz + facingNormal * 1.0e-3;
		emitterMisValid = primaryParams.emitterCount > 0u;

        var nextRay: PathTracerRay;
        nextRay.originMin = vec4<f32>(hit.positionT.xyz + facingNormal * 1.0e-3, 1.0e-4);
        nextRay.directionMax = vec4<f32>(incoming, 1.0e30);
        nextRay.skipCellGrid = hit.cellGrid;
        nextRay.flags = vec4<u32>(0u);
        let nextHit = traceTransportScene(nextRay, primaryParams.gridCount);
        ray = nextRay;
        hit = nextHit;
    }
	let clampValue = max(lightingData.environmentParams.z, 1.0);
	let maximumRadiance = max(radiance.x, max(radiance.y, radiance.z));
	if (maximumRadiance > clampValue) {
		radiance *= clampValue / maximumRadiance;
	}
    output.radianceAlpha = vec4<f32>(radiance, pathAlpha);
    let luminance = dot(radiance, vec3<f32>(0.2126, 0.7152, 0.0722));
    output.moments.x = luminance * luminance;
    return output;
}

fn accumulateSample(previous: PathTracerSampleOutput,
                    sample: PathTracerSampleOutput) -> PathTracerSampleOutput {
    var accumulated = sample;
    accumulated.radianceAlpha += previous.radianceAlpha;
    accumulated.albedoFeature += previous.albedoFeature;
    accumulated.normalDepth += previous.normalDepth;
    accumulated.positionOpacity += previous.positionOpacity;
    accumulated.moments += previous.moments;
    return accumulated;
}

fn clampSampleHistory(previous: PathTracerSampleOutput,
					  sample: PathTracerSampleOutput) -> PathTracerSampleOutput {
	var clamped = sample;
	let historyCount = max(previous.moments.y, 1.0);
	let historyMeanColor = previous.radianceAlpha.xyz / historyCount;
	let historyMean = dot(historyMeanColor, vec3<f32>(0.2126, 0.7152, 0.0722));
	let historyVariance = max(previous.moments.x / historyCount - historyMean * historyMean, 0.0);
	let sampleLuminance = dot(sample.radianceAlpha.xyz, vec3<f32>(0.2126, 0.7152, 0.0722));
	let statisticalLimit = historyMean + 10.0 * sqrt(historyVariance) + 0.25;
	let relativeLimit = historyMean * 6.0 + 0.5;
	let historyLimit = max(statisticalLimit, relativeLimit);
	if (sampleLuminance > historyLimit) {
		clamped.radianceAlpha = vec4<f32>(sample.radianceAlpha.xyz *
			(historyLimit / max(sampleLuminance, 1.0e-6)), sample.radianceAlpha.w);
		let luminance = dot(clamped.radianceAlpha.xyz, vec3<f32>(0.2126, 0.7152, 0.0722));
		clamped.moments.x = luminance * luminance;
	}
	return clamped;
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) invocation: vec3<u32>) {
    let rayIndex = invocation.x;
    if (rayIndex >= dispatchParams.rayCount) {
        return;
    }
    let ray = rays[rayIndex];
    hits[rayIndex] = traceScene(ray, dispatchParams.gridCount);
}

fn pixelConverged(previous: PathTracerSampleOutput, count: u32, errorTolerance: f32) -> bool {
    let countF = f32(count);
    let historyMeanColor = previous.radianceAlpha.xyz / countF;
    let historyMean = dot(historyMeanColor, vec3<f32>(0.2126, 0.7152, 0.0722));
    let variance = max(previous.moments.x / countF - historyMean * historyMean, 0.0);
    let standardError = sqrt(variance / countF);
    let relativeError = standardError / max(historyMean, 0.01);
    return relativeError <= errorTolerance;
}

@compute @workgroup_size(64)
fn primaryMain(@builtin(global_invocation_id) invocation: vec3<u32>) {
    let pixelIndex = invocation.x;
    if (pixelIndex >= primaryParams.pixelCount) {
        return;
    }
    let count = u32(sampleOutputs[pixelIndex].moments.y + 0.5);
    // Adaptive sampling: stop a pixel once its accumulated mean is stable so it
    // no longer receives rays. The Sobol sequence index stays
    // primaryParams.sampleIndex (the global pass count), so an active pixel's
    // samples are bit-identical to the non-adaptive schedule.
    if (primaryParams.adaptiveEnabled != 0u && count >= primaryParams.adaptiveMinSamples &&
        pixelConverged(sampleOutputs[pixelIndex], count, primaryParams.adaptiveError)) {
        return;
    }
    let ray = primaryRay(pixelIndex);
    let closest = traceTransportScene(ray, primaryParams.gridCount);
    hits[pixelIndex] = closest;
	var sample = shadePrimary(pixelIndex, ray, closest);
	if (count >= 16u) {
		sample = clampSampleHistory(sampleOutputs[pixelIndex], sample);
	}
    if (count == 0u) {
        sampleOutputs[pixelIndex] = sample;
    } else {
        sampleOutputs[pixelIndex] = accumulateSample(sampleOutputs[pixelIndex], sample);
    }
}
)WGSL"
R"WGSL(
// ---------------------------------------------------------------------------
// Denoise + tonemap compute pass (roadmap item 5, deferred perf optimization).
// Runs the edge-aware a-trous spatial filter and the single linear->sRGB
// output transform on the GPU so the only thing read back is the packed RGBA8
// final image (4 B/px) instead of the 96-byte guide struct. Faithful port of
// VoxelDDAPathTracer::denoiseColor (spatial only) + pathTracerTonemap.

struct PathTracerDenoiseParams {
    dimensions: vec4<u32>,      // width, height, reserved, reserved
    exposure: vec4<f32>,        // exposure (stops), reserved, reserved, reserved
    modeStepFilmic: vec4<u32>,  // mode (0 init / 1 filter / 2 tonemap / 3 temporal), step, filmic, seed
};

// Persistent per-pixel temporal history (denoiseTemporal). Color is stored
// feature-removed: the edge factor factors out of the linear blend and is
// re-applied once in the tonemap, which is equivalent to the CPU for a static
// camera (camera motion resets history anyway).
struct TemporalHistory {
    color: vec4<f32>,        // rgb + reserved
    normal: vec4<f32>,       // xyz + reserved
    albedoDepth: vec4<f32>,  // albedo.xyz + depth
    count: vec4<f32>,        // history count + reserved x3
};

@group(0) @binding(0) var<storage, read> denoiseOutputs: array<PathTracerSampleOutput>;
@group(0) @binding(1) var<storage, read> denoisePing: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read_write> denoisePong: array<vec4<f32>>;
@group(0) @binding(3) var<uniform> denoiseParams: PathTracerDenoiseParams;
@group(0) @binding(4) var<storage, read_write> finalImage: array<u32>;
@group(0) @binding(5) var<uniform> denoiseCamera: PathTracerCameraData;
@group(0) @binding(6) var<uniform> denoisePreviousVP: mat4x4<f32>;
@group(0) @binding(7) var<storage, read> temporalHistory: array<TemporalHistory>;
@group(0) @binding(8) var<storage, read_write> temporalNext: array<TemporalHistory>;

struct DenoiseGuides {
    albedo: vec3<f32>,
    normal: vec3<f32>,
    depth: f32,
    feature: f32,
    alpha: f32,
    variance: f32,
    edgeFactor: f32,
    meanColor: vec3<f32>,
};

fn denoiseFeature(output: PathTracerSampleOutput) -> f32 {
    return output.albedoFeature.w / max(output.moments.y, 1.0);
}

// Normalize the accumulated sample output by its own sample count (adaptive
// sampling gives every pixel a different count; moments.y is authoritative).
fn denoiseGuides(output: PathTracerSampleOutput) -> DenoiseGuides {
    var guides: DenoiseGuides;
    let count = max(output.moments.y, 1.0);
    let scale = 1.0 / count;
    guides.meanColor = output.radianceAlpha.xyz * scale;
    guides.albedo = output.albedoFeature.xyz * scale;
    var normal = output.normalDepth.xyz;
    let normalLength = length(normal);
    if (normalLength > 1.0e-6) {
        normal = normal / normalLength;
    }
    guides.normal = normal;
    guides.depth = output.normalDepth.w * scale;
    guides.alpha = clamp(output.radianceAlpha.w * scale, 0.0, 1.0);
    guides.feature = denoiseFeature(output);
    let luma = dot(guides.meanColor, vec3<f32>(0.2126, 0.7152, 0.0722));
    let sampleVariance = max(output.moments.x * scale - luma * luma, 0.0);
    guides.variance = sampleVariance * scale;
    guides.edgeFactor = clamp(guides.feature, 0.62, 1.02);
    return guides;
}

// IEC 61966-2-1 sRGB encode (PathTracerTonemap.h pathTracerSrgbEncode).
fn denoiseSrgbEncode(linear: f32) -> f32 {
    let clamped = max(linear, 0.0);
    if (clamped <= 0.0031308) {
        return 12.92 * clamped;
    }
    return 1.055 * pow(clamped, 1.0 / 2.4) - 0.055;
}

// ACES filmic approximation, Knarkowicz 2016 (pathTracerFilmic).
fn denoiseFilmic(linear: vec3<f32>) -> vec3<f32> {
    let hdr = linear * 0.6;
    let ldr = (hdr * hdr * 2.51 + hdr * 0.03) / (hdr * hdr * 2.43 + hdr * 0.59 + 0.14);
    return max(ldr, vec3<f32>(0.0));
}

fn denoiseTonemap(radiance: vec3<f32>, exposure: f32, filmic: u32) -> vec3<f32> {
    var rgb = radiance;
    if (exposure != 0.0) {
        rgb = rgb * exp2(exposure);
    }
    if (filmic != 0u) {
        rgb = denoiseFilmic(rgb);
    }
    return vec3<f32>(denoiseSrgbEncode(rgb.x), denoiseSrgbEncode(rgb.y), denoiseSrgbEncode(rgb.z));
}

// Reconstruct a pixel's world position from its depth, mirroring
// pathTracerCameraRay(camera, x+0.5, y+0.5) * depth (denoiseTemporal).
fn denoiseWorldPosition(camera: PathTracerCameraData, depth: f32, x: u32, y: u32) -> vec3<f32> {
    let pixelX = f32(x) + 0.5;
    let pixelY = f32(y) + 0.5;
    let ndcX = pixelX * camera.viewport.z * 2.0 - 1.0;
    let ndcY = 1.0 - pixelY * camera.viewport.w * 2.0;
    let near = camera.inverseViewProjection * vec4<f32>(ndcX, ndcY, -1.0, 1.0);
    let far = camera.inverseViewProjection * vec4<f32>(ndcX, ndcY, 1.0, 1.0);
    let nearPoint = near.xyz / near.w;
    let farPoint = far.xyz / far.w;
    let origin = nearPoint;
    let direction = normalize(farPoint - nearPoint);
    return origin + direction * depth;
}

@compute @workgroup_size(64)
fn denoiseMain(@builtin(global_invocation_id) invocation: vec3<u32>) {
    let pixelIndex = invocation.x;
    let width = denoiseParams.dimensions.x;
    let height = denoiseParams.dimensions.y;
    let pixelCount = width * height;
    if (pixelIndex >= pixelCount) {
        return;
    }
    let mode = denoiseParams.modeStepFilmic.x;
    let x = pixelIndex % width;
    let y = pixelIndex / width;

    if (mode == 0u) {
        // Init: normalize and remove the studio-bevel feature so the analytic
        // seam survives the spatial passes (mirrors denoiseColor's edgeFactor
        // divide before the a-trous passes).
        let guides = denoiseGuides(denoiseOutputs[pixelIndex]);
        denoisePong[pixelIndex] = vec4<f32>(guides.meanColor / guides.edgeFactor, 0.0);
        return;
    }

    if (mode == 2u) {
        // Tonemap: re-apply the feature removed before the passes, apply the
        // single output transform, and pack RGBA8 (matches image()).
        let guides = denoiseGuides(denoiseOutputs[pixelIndex]);
        let color = denoisePing[pixelIndex].xyz * guides.edgeFactor;
        let ldr = denoiseTonemap(color, denoiseParams.exposure.x, denoiseParams.modeStepFilmic.z);
        let r = u32(clamp(ldr.x * 255.0, 0.0, 255.0));
        let g = u32(clamp(ldr.y * 255.0, 0.0, 255.0));
        let b = u32(clamp(ldr.z * 255.0, 0.0, 255.0));
        let a = u32(clamp(guides.alpha * 255.0, 0.0, 255.0));
        finalImage[pixelIndex] = r | (g << 8u) | (b << 16u) | (a << 24u);
        return;
    }

    if (mode == 3u) {
        // Temporal accumulation (denoiseTemporal): reproject the pixel into the
        // previous frame's history, reject on normal/depth/albedo inconsistency,
        // then blend with a history-count-capped EMA. Reads the spatial result
        // from denoisePing and writes the blended color to denoisePong.
        let guides = denoiseGuides(denoiseOutputs[pixelIndex]);
        let spatialColor = denoisePing[pixelIndex].xyz;
        let seed = denoiseParams.modeStepFilmic.w != 0u;
        var history: TemporalHistory;
        history.color = vec4<f32>(spatialColor, 0.0);
        history.normal = vec4<f32>(guides.normal, 0.0);
        history.albedoDepth = vec4<f32>(guides.albedo, guides.depth);
        history.count = vec4<f32>(0.0, 0.0, 0.0, 0.0);

        // First temporal frame (or a reset): seed history and return unchanged.
        if (seed) {
            history.count = vec4<f32>(1.0, 0.0, 0.0, 0.0);
            denoisePong[pixelIndex] = vec4<f32>(spatialColor, 0.0);
            temporalNext[pixelIndex] = history;
            return;
        }

        // Transparent / background pixels keep the current color and reset
        // history (there is no surface to reproject).
        if (guides.alpha < 1.0e-3 || guides.depth <= 0.0) {
            denoisePong[pixelIndex] = vec4<f32>(spatialColor, 0.0);
            temporalNext[pixelIndex] = history;
            return;
        }

        // Motion-vector reprojection: world position from the current camera
        // ray + depth, projected through the previous frame's view-projection.
        let world = denoiseWorldPosition(denoiseCamera, guides.depth, x, y);
        let previousClip = denoisePreviousVP * vec4<f32>(world, 1.0);
        var prevX = f32(x) + 0.5;
        var prevY = f32(y) + 0.5;
        if (previousClip.w > 1.0e-6) {
            let ndcX = previousClip.x / previousClip.w;
            let ndcY = previousClip.y / previousClip.w;
            prevX = (ndcX * 0.5 + 0.5) * f32(width);
            prevY = (1.0 - ndcY * 0.5) * f32(height);
        }
        let inBounds = prevX >= 0.0 && prevX < f32(width) && prevY >= 0.0 && prevY < f32(height);
        if (!inBounds) {
            denoisePong[pixelIndex] = vec4<f32>(spatialColor, 0.0);
            history.count = vec4<f32>(1.0, 0.0, 0.0, 0.0);
            temporalNext[pixelIndex] = history;
            return;
        }
        let px = clamp(i32(floor(prevX)), 0, i32(width) - 1);
        let py = clamp(i32(floor(prevY)), 0, i32(height) - 1);
        let pi = u32(py) * width + u32(px);

        // History rejection: normal, depth, and albedo consistency (SVGF).
        let histColor = temporalHistory[pi].color.xyz;
        let histNormal = temporalHistory[pi].normal.xyz;
        let histAlbedoDepth = temporalHistory[pi].albedoDepth;
        let histCount = temporalHistory[pi].count.x;
        let ndot = clamp(dot(guides.normal, histNormal), 0.0, 1.0);
        let normalWeight = pow(ndot, 32.0);
        let depthScale = 0.03 * max(guides.depth, histAlbedoDepth.w) + 0.01;
        let depthDelta = abs(guides.depth - histAlbedoDepth.w) / depthScale;
        let depthWeight = exp(-depthDelta * depthDelta);
        let albedoDelta = guides.albedo - histAlbedoDepth.xyz;
        let albedoWeight = exp(-dot(albedoDelta, albedoDelta) * 80.0);
        let rejectWeight = normalWeight * depthWeight * albedoWeight;

        let newCount = select(0.0, min(histCount + 1.0, 32.0), rejectWeight > 0.1);
        let temporalAlpha = 1.0 / (1.0 + newCount);
        let blended = mix(histColor, spatialColor, temporalAlpha);

        denoisePong[pixelIndex] = vec4<f32>(blended, 0.0);
        history.color = vec4<f32>(blended, 0.0);
        history.count = vec4<f32>(newCount, 0.0, 0.0, 0.0);
        temporalNext[pixelIndex] = history;
        return;
    }

    // mode == 1: one edge-aware a-trous pass at the uniform's step (1, 2, 4).
    let step = denoiseParams.modeStepFilmic.y;
    let center = denoiseGuides(denoiseOutputs[pixelIndex]);
    if (center.alpha < 1.0e-3) {
        denoisePong[pixelIndex] = denoisePing[pixelIndex];
        return;
    }
    let centerColor = denoisePing[pixelIndex].xyz;
    let luma0 = dot(centerColor, vec3<f32>(0.2126, 0.7152, 0.0722));
    let kernel = vec3<f32>(1.0, 2.0, 1.0);
    var sum = vec3<f32>(0.0);
    var sumWeight = 0.0;
    for (var ky = -1; ky <= 1; ky += 1) {
        for (var kx = -1; kx <= 1; kx += 1) {
            let nx = i32(x) + kx * i32(step);
            let ny = i32(y) + ky * i32(step);
            if (nx < 0 || ny < 0 || nx >= i32(width) || ny >= i32(height)) {
                continue;
            }
            let j = u32(ny) * width + u32(nx);
            let neighbor = denoiseGuides(denoiseOutputs[j]);
            let neighborColor = denoisePing[j].xyz;
            let albedoDelta = center.albedo - neighbor.albedo;
            let albedoWeight = exp(-dot(albedoDelta, albedoDelta) * 80.0);
            let nlen0 = dot(center.normal, center.normal);
            let nlen1 = dot(neighbor.normal, neighbor.normal);
            var normalWeight = 1.0;
            if (nlen0 > 1.0e-12 && nlen1 > 1.0e-12) {
                let ndot = clamp(dot(center.normal, neighbor.normal), 0.0, 1.0);
                normalWeight = pow(ndot, 32.0);
            } else if (nlen0 > 1.0e-12 || nlen1 > 1.0e-12) {
                normalWeight = 0.0;
            }
            let depthScale = 0.03 * max(center.depth, neighbor.depth) + 0.01;
            let depthDelta = abs(center.depth - neighbor.depth) / depthScale;
            let depthWeight = exp(-depthDelta * depthDelta);
            let featureDelta = center.feature - neighbor.feature;
            let featureWeight = exp(-featureDelta * featureDelta * 1000.0);
            var featureBarrierWeight = 1.0;
            if (step > 1u && (kx != 0 || ky != 0)) {
                var minFeature = 1.0;
                for (var s = 1; s < i32(step); s += 1) {
                    let sx = i32(x) + kx * s;
                    let sy = i32(y) + ky * s;
                    let si = u32(sy) * width + u32(sx);
                    minFeature = min(minFeature, denoiseFeature(denoiseOutputs[si]));
                }
                let barrier = 1.0 - minFeature;
                featureBarrierWeight = exp(-barrier * barrier * 64.0);
            }
            let alphaDelta = center.alpha - neighbor.alpha;
            let alphaWeight = exp(-alphaDelta * alphaDelta * 64.0);
            let luma1 = dot(neighborColor, vec3<f32>(0.2126, 0.7152, 0.0722));
            let colorSigma = 1.5 * sqrt(center.variance + neighbor.variance) + 0.025 * max(luma0, luma1) + 0.002;
            let lumaDelta = (luma0 - luma1) / colorSigma;
            let colorWeight = exp(-0.125 * lumaDelta * lumaDelta);
            let spatialWeight = kernel[u32(kx + 1)] * kernel[u32(ky + 1)];
            let weight = spatialWeight * albedoWeight * normalWeight * depthWeight * featureWeight *
                featureBarrierWeight * alphaWeight * colorWeight;
            sum += neighborColor * weight;
            sumWeight += weight;
        }
    }
    let invWeight = select(1.0, 1.0 / sumWeight, sumWeight > 1.0e-8);
    denoisePong[pixelIndex] = vec4<f32>(sum * invWeight, 0.0);
}

// ---------------------------------------------------------------------------
// Adaptive-sampling convergence flag (roadmap item 3, step 3). A separate pass
// that atomically counts still-unconverged pixels into one 4-byte counter, so
// the driver can stop dispatching from a single read instead of reading back
// the full per-pixel moments. Reuses pixelConverged (same predicate as the
// primaryMain early-out), so the flag and the skip decision agree exactly.

@group(0) @binding(0) var<storage, read> convergenceOutputs: array<PathTracerSampleOutput>;
@group(0) @binding(1) var<uniform> convergenceParams: PathTracerPrimaryParams;
@group(0) @binding(2) var<storage, read_write> unconvergedCount: array<atomic<u32>>;

@compute @workgroup_size(64)
fn convergenceMain(@builtin(global_invocation_id) invocation: vec3<u32>) {
    let pixelIndex = invocation.x;
    if (pixelIndex >= convergenceParams.pixelCount) {
        return;
    }
    let count = u32(convergenceOutputs[pixelIndex].moments.y + 0.5);
    let converged = convergenceParams.adaptiveEnabled != 0u &&
        count >= convergenceParams.adaptiveMinSamples &&
        pixelConverged(convergenceOutputs[pixelIndex], count, convergenceParams.adaptiveError);
    if (!converged) {
        atomicAdd(&unconvergedCount[0], 1u);
    }
}
)WGSL";
}

} // namespace voxelpathtracer
