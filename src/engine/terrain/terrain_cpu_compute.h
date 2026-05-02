#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine
{

// Decode raw BGRA/RGBA heightfield bytes (matching cs_heightfield_minmax.sc encoding rules),
// compute min/max over valid pixels, then write a normalized R32F (linear 0..1, quantized
// to 16-bit steps) blob suitable for upload as bgfx::TextureFormat::R32F.
//
// decodeMode: 0 = int32 packed (value /100), 1 = float32 packed.
// orderId:     0..3 channel reorder selector inside cs_heightfield_minmax.sc
// rawIsBGRA:   true if raw is BGRA8, false if RGBA8.
//
// Returns true on success. On success outR32F has width*height floats in [0,1].
// Sentinel input values (-1000000 / 1000000 / -500000) are treated as invalid (NaN);
// they collapse to the lower bound of the valid range in the normalized output.
bool cpuDecodeAndNormalizeHeightfield(const uint8_t* rawBytes,
                                    int width,
                                    int height,
                                    int decodeMode,
                                    int orderId,
                                    bool rawIsBGRA,
                                    std::vector<float>& outR32F,
                                    float& outMin,
                                    float& outMax);

// Compute a robust min/max range for a decoded height array, ignoring outlier
// clusters separated from the main distribution by histogram gaps.
//
// Some inspection platforms (e.g. 430) emit pixels saturated near int16 minimum
// (≈ -32000) or other internal sentinel clusters that aren't in the canonical
// (-1e6 / +1e6 / -5e5) sentinel list. A naive min/max picks up these outliers
// and crushes the useful 3D height signal into a tiny normalized band, making
// the rendered geometry look flat.
//
// Algorithm: iteratively detect a histogram gap (≥5% of bins empty in a run)
// from each side and discard the leading/trailing cluster as long as it
// represents ≤5% of finite pixels. Returns the cleaned [outMin, outMax] range
// covering the bulk of the height distribution.
//
// finiteValues:    pointer to height values (NaN/inf entries are ignored).
// count:           number of entries pointed to.
// outMin / outMax: result range; both 0 if no finite samples.
// Returns true if at least one finite value was found.
bool computeRobustHeightRange(const float* finiteValues,
                              std::size_t count,
                              float& outMin,
                              float& outMax);

// Generate the slope map (rgba32f), matching cs_generate_smap.sc.
// dmapNormalized: width*height floats in [0,1] (the R32F dmap data).
// outRGBA32F:     resized to width*height*4 floats. Channels: (slope_x, slope_y, 0, 0).
void cpuGenerateSmap(const float* dmapNormalized,
                     int width,
                     int height,
                     std::vector<float>& outRGBA32F);

// Compute the maximum dmap-derived height inside each rect, mirroring
// cs_overlay_max_elevation.sc. rects layout matches the GPU buffer: 2 vec4 per rect
// (rect0 = (p0x, p0y, ux, uy), rect1 = (vx, vy, _, _)).
//
// dmapNormalized: width*height floats in [0,1].
// terrainHalfWidth/Height, dmapFactor, dmapBias: same uniforms as the shader.
//
// outMaxHeights resized to rectCount; entries are world-space height (after
// dmapFactor*norm + dmapBias). Empty u or v vector → 0.
void cpuComputeRectMax(const float* rects,
                       int rectCount,
                       const float* dmapNormalized,
                       int dmapWidth,
                       int dmapHeight,
                       float terrainHalfWidth,
                       float terrainHalfHeight,
                       float dmapFactor,
                       float dmapBias,
                       std::vector<float>& outMaxHeights);

} // namespace engine
