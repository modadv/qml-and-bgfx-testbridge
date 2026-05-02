#include "terrain_cpu_compute.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace engine
{
namespace
{
    inline bool isInvalidSentinel(float v)
    {
        return v == -1000000.0f || v == 1000000.0f || v == -500000.0f;
    }

    inline bool isValidFloat(float v)
    {
        return std::isfinite(v) && std::fabs(v) < 1.0e20f;
    }

    inline float decodePixel(const uint8_t* px,
                             int decodeMode,
                             int orderId,
                             bool rawIsBGRA)
    {
        // px points to 4 bytes. Layout convention follows cs_heightfield_minmax.sc:
        //   raw is treated as BGRA8 or RGBA8 sample → ordering selector applies.
        const uint8_t r = px[rawIsBGRA ? 2 : 0];
        const uint8_t g = px[1];
        const uint8_t b = px[rawIsBGRA ? 0 : 2];
        const uint8_t a = px[3];

        // Match the shader: b0=B (or R if !BGRA), b1=G, b2=R (or B), b3=A.
        uint32_t b0 = b;
        uint32_t b1 = g;
        uint32_t b2 = r;
        uint32_t b3 = a;

        uint32_t o0 = b0, o1 = b1, o2 = b2, o3 = b3;
        if (orderId == 1)        { o0 = b2; o1 = b1; o2 = b0; o3 = b3; }
        else if (orderId == 2)   { o0 = b3; o1 = b2; o2 = b1; o3 = b0; }
        else if (orderId == 3)   { o0 = b3; o1 = b0; o2 = b1; o3 = b2; }

        const uint32_t packed = o0 | (o1 << 8) | (o2 << 16) | (o3 << 24);

        if (decodeMode == 0)
        {
            const int32_t v = static_cast<int32_t>(packed);
            if (v == -1000000 || v == 1000000 || v == -500000)
                return std::numeric_limits<float>::quiet_NaN();
            return float(v) / 100.0f;
        }
        else
        {
            float f;
            std::memcpy(&f, &packed, sizeof(float));
            if (isInvalidSentinel(f))
                return std::numeric_limits<float>::quiet_NaN();
            return f;
        }
    }
}

bool cpuDecodeAndNormalizeHeightfield(const uint8_t* rawBytes,
                                    int width,
                                    int height,
                                    int decodeMode,
                                    int orderId,
                                    bool rawIsBGRA,
                                    std::vector<float>& outR32F,
                                    float& outMin,
                                    float& outMax)
{
    if (rawBytes == nullptr || width <= 0 || height <= 0)
        return false;

    const size_t pixelCount = size_t(width) * size_t(height);
    std::vector<float> heights(pixelCount, std::numeric_limits<float>::quiet_NaN());

    float minVal = std::numeric_limits<float>::infinity();
    float maxVal = -std::numeric_limits<float>::infinity();

    for (size_t i = 0; i < pixelCount; ++i)
    {
        const uint8_t* px = rawBytes + i * 4;
        const float h = decodePixel(px, decodeMode, orderId & 3, rawIsBGRA);
        heights[i] = h;
        if (isValidFloat(h))
        {
            if (h < minVal) minVal = h;
            if (h > maxVal) maxVal = h;
        }
    }

    if (!isValidFloat(minVal) || !isValidFloat(maxVal) || maxVal <= minVal)
    {
        outR32F.assign(pixelCount, 0.0f);
        outMin = 0.0f;
        outMax = 0.0f;
        return true;
    }

    outMin = minVal;
    outMax = maxVal;

    const bool shift = minVal < 0.0f;
    const float range = shift ? (maxVal - minVal) : maxVal;
    if (range <= 0.0f)
    {
        outR32F.assign(pixelCount, 0.0f);
        return true;
    }

    const float invalidNorm = shift ? std::min(1.0f, std::max(0.0f, -minVal / range)) : 0.0f;

    outR32F.resize(pixelCount);
    for (size_t i = 0; i < pixelCount; ++i)
    {
        const float h = heights[i];
        float norm = invalidNorm;
        if (isValidFloat(h))
        {
            const float shifted = shift ? (h - minVal) : h;
            norm = std::min(1.0f, std::max(0.0f, shifted / range));
        }
        const float quantized = std::floor(norm * 65535.0f) / 65535.0f;
        outR32F[i] = quantized;
    }
    return true;
}

bool computeRobustHeightRange(const float* finiteValues,
                              std::size_t count,
                              float& outMin,
                              float& outMax)
{
    outMin = 0.0f;
    outMax = 0.0f;
    if (finiteValues == nullptr || count == 0)
        return false;

    float rawMin = std::numeric_limits<float>::infinity();
    float rawMax = -std::numeric_limits<float>::infinity();
    size_t finiteCount = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const float v = finiteValues[i];
        if (!isValidFloat(v))
            continue;
        rawMin = std::min(rawMin, v);
        rawMax = std::max(rawMax, v);
        ++finiteCount;
    }
    if (finiteCount == 0)
        return false;

    outMin = rawMin;
    outMax = rawMax;
    if (finiteCount < 100u || rawMax <= rawMin)
        return true;

    // Iterative gap-detection: shrink [curMin, curMax] by trimming sparse
    // outlier clusters from either end. The total trimmed pixel budget is
    // capped at 5% per side across all iterations combined (each iteration
    // decrements its remaining budget by what it just trimmed).
    constexpr int    kBinCount       = 256;
    constexpr int    kGapBinsMin     = 13;     // ≈5% of bins
    constexpr double kOutlierMaxFrac = 0.05;   // ≤5% of pixels per side total
    constexpr int    kMaxIters       = 4;

    float curMin = rawMin;
    float curMax = rawMax;
    size_t leftBudget  = static_cast<size_t>(double(finiteCount) * kOutlierMaxFrac);
    size_t rightBudget = leftBudget;

    for (int iter = 0; iter < kMaxIters; ++iter)
    {
        const float range = curMax - curMin;
        if (range <= 0.0f)
            break;

        std::vector<size_t> hist(kBinCount, 0);
        const float invRange = float(kBinCount) / range;
        for (size_t i = 0; i < count; ++i)
        {
            const float v = finiteValues[i];
            if (!isValidFloat(v) || v < curMin || v > curMax)
                continue;
            int idx = int((v - curMin) * invRange);
            if (idx < 0) idx = 0;
            else if (idx >= kBinCount) idx = kBinCount - 1;
            hist[size_t(idx)] += 1;
        }

        float newMin = curMin;
        float newMax = curMax;
        size_t leftTrimmed  = 0;
        size_t rightTrimmed = 0;

        // Walk from the left, trimming if the leading cluster fits in the
        // remaining left-side outlier budget.
        size_t cumLeft = 0;
        int    emptyRun = 0;
        for (int i = 0; i < kBinCount; ++i)
        {
            if (hist[size_t(i)] > 0)
            {
                if (emptyRun >= kGapBinsMin && cumLeft <= leftBudget && cumLeft > 0)
                {
                    newMin = curMin + (range * float(i) / float(kBinCount));
                    leftTrimmed = cumLeft;
                    break;
                }
                emptyRun = 0;
                cumLeft += hist[size_t(i)];
            }
            else
            {
                ++emptyRun;
            }
        }

        // Walk from the right.
        size_t cumRight = 0;
        emptyRun = 0;
        for (int i = kBinCount - 1; i >= 0; --i)
        {
            if (hist[size_t(i)] > 0)
            {
                if (emptyRun >= kGapBinsMin && cumRight <= rightBudget && cumRight > 0)
                {
                    newMax = curMin + (range * float(i + 1) / float(kBinCount));
                    rightTrimmed = cumRight;
                    break;
                }
                emptyRun = 0;
                cumRight += hist[size_t(i)];
            }
            else
            {
                ++emptyRun;
            }
        }

        if (newMin <= curMin && newMax >= curMax)
            break;
        // Sanity: never collapse the range to zero.
        if (newMax <= newMin)
            break;
        curMin = newMin;
        curMax = newMax;
        leftBudget  = (leftTrimmed  < leftBudget)  ? (leftBudget  - leftTrimmed)  : 0;
        rightBudget = (rightTrimmed < rightBudget) ? (rightBudget - rightTrimmed) : 0;
        if (leftBudget == 0 && rightBudget == 0)
            break;
    }

    outMin = curMin;
    outMax = curMax;
    return true;
}

void cpuGenerateSmap(const float* dmap,
                     int width,
                     int height,
                     std::vector<float>& outRGBA32F)
{
    outRGBA32F.assign(size_t(width) * size_t(height) * 4u, 0.0f);
    if (dmap == nullptr || width <= 0 || height <= 0)
        return;

    auto sample = [dmap, width, height](int x, int y) -> float {
        x = std::min(width  - 1, std::max(0, x));
        y = std::min(height - 1, std::max(0, y));
        return dmap[size_t(y) * size_t(width) + size_t(x)];
    };

    const float fw = float(width);
    const float fh = float(height);

    for (int j = 0; j < height; ++j)
    {
        for (int i = 0; i < width; ++i)
        {
            const float zL = sample(i - 1, j);
            const float zR = sample(i + 1, j);
            const float zB = sample(i, j - 1);
            const float zT = sample(i, j + 1);
            const float slopeX = fw * 0.5f * (zR - zL);
            const float slopeY = fh * 0.5f * (zT - zB);

            float* dst = &outRGBA32F[(size_t(j) * size_t(width) + size_t(i)) * 4u];
            dst[0] = slopeX;
            dst[1] = slopeY;
            dst[2] = 0.0f;
            dst[3] = 0.0f;
        }
    }
}

void cpuComputeRectMax(const float* rects,
                       int rectCount,
                       const float* dmap,
                       int dmapWidth,
                       int dmapHeight,
                       float terrainHalfWidth,
                       float terrainHalfHeight,
                       float dmapFactor,
                       float dmapBias,
                       std::vector<float>& outMaxHeights)
{
    outMaxHeights.assign(size_t(std::max(0, rectCount)), 0.0f);
    if (rects == nullptr || rectCount <= 0 || dmap == nullptr ||
        dmapWidth <= 0 || dmapHeight <= 0 ||
        terrainHalfWidth <= 0.0f || terrainHalfHeight <= 0.0f)
    {
        return;
    }

    auto sample = [dmap, dmapWidth, dmapHeight](float u, float v) -> float {
        u = std::min(1.0f, std::max(0.0f, u));
        v = std::min(1.0f, std::max(0.0f, v));
        const int x = std::min(dmapWidth  - 1, int(u * float(dmapWidth)));
        const int y = std::min(dmapHeight - 1, int(v * float(dmapHeight)));
        return dmap[size_t(y) * size_t(dmapWidth) + size_t(x)];
    };

    constexpr int kGrid = 16;
    const float invGrid = 1.0f / float(kGrid);

    for (int r = 0; r < rectCount; ++r)
    {
        const float* r0 = rects + size_t(r) * 8;
        const float* r1 = r0 + 4;
        const float p0x = r0[0];
        const float p0y = r0[1];
        const float ux  = r0[2];
        const float uy  = r0[3];
        const float vx  = r1[0];
        const float vy  = r1[1];

        const float uLen2 = ux * ux + uy * uy;
        const float vLen2 = vx * vx + vy * vy;
        if (uLen2 <= 1e-8f || vLen2 <= 1e-8f)
        {
            outMaxHeights[r] = 0.0f;
            continue;
        }

        float maxH = -std::numeric_limits<float>::infinity();
        for (int sy = 0; sy < kGrid; ++sy)
        {
            for (int sx = 0; sx < kGrid; ++sx)
            {
                const float tx = (float(sx) + 0.5f) * invGrid;
                const float ty = (float(sy) + 0.5f) * invGrid;

                const float wx = p0x + tx * ux + ty * vx;
                const float wy = p0y + tx * uy + ty * vy;

                const float u = (wx + terrainHalfWidth)  / (2.0f * terrainHalfWidth);
                const float v = (wy + terrainHalfHeight) / (2.0f * terrainHalfHeight);

                const float h = sample(u, v) * dmapFactor + dmapBias;
                if (h > maxH) maxH = h;
            }
        }

        outMaxHeights[r] = std::isfinite(maxH) ? maxH : 0.0f;
    }
}

} // namespace engine
