// terrain_renderer_overlay.cpp
//
// Overlay subsystem of TerrainRenderer: 2D overlay rectangles, screen/world-space
// transforms, picking, and GPU/CPU max-elevation readback. Split out of the
// TerrainRenderer god class; implements the same methods declared in
// terrain_renderer.h.

#include "terrain_renderer.h"
#include "terrain_types.h"
#include "terrain_renderer_internal.h"
#include "terrain_cpu_compute.h"
#include "logger.h"
#include "render_device.h"
#include "render_capabilities.h"

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

uint16_t nextPow2(uint16_t value)
{
    uint16_t pow2 = 1;
    while (pow2 < value)
    {
        pow2 <<= 1;
    }
    return pow2;
}

bool screenToLocalPoint(float sx, float sy, float viewW, float viewH, float ndcNear,
                        const float* invViewProj, const float* invModel, bx::Vec3& outLocal)
{
    if (viewW <= 0.0f || viewH <= 0.0f)
    {
        return false;
    }

    const float ndcX = (sx / viewW) * 2.0f - 1.0f;
    const float ndcY = ((viewH - sy) / viewH) * 2.0f - 1.0f;

    const bx::Vec3 p0 = bx::mulH({ ndcX, ndcY, ndcNear }, invViewProj);
    const bx::Vec3 p1 = bx::mulH({ ndcX, ndcY, 1.0f }, invViewProj);

    const bx::Vec3 p0l = bx::mul(p0, invModel);
    const bx::Vec3 p1l = bx::mul(p1, invModel);
    const bx::Vec3 dir = bx::sub(p1l, p0l);

    if (std::fabs(dir.z) < 1.0e-6f)
    {
        return false;
    }

    const float t = -p0l.z / dir.z;
    outLocal = bx::add(p0l, bx::mul(dir, t));
    return true;
}

bool screenToLocalRay(float sx, float sy, float viewW, float viewH, float ndcNear,
                      const float* invViewProj, const float* invModel,
                      bx::Vec3& outOrigin, bx::Vec3& outDir)
{
    if (viewW <= 0.0f || viewH <= 0.0f)
    {
        return false;
    }

    const float ndcX = (sx / viewW) * 2.0f - 1.0f;
    const float ndcY = ((viewH - sy) / viewH) * 2.0f - 1.0f;

    const bx::Vec3 p0 = bx::mulH({ ndcX, ndcY, ndcNear }, invViewProj);
    const bx::Vec3 p1 = bx::mulH({ ndcX, ndcY, 1.0f }, invViewProj);

    outOrigin = bx::mul(p0, invModel);
    const bx::Vec3 p1l = bx::mul(p1, invModel);
    outDir = bx::sub(p1l, outOrigin);

    const float len2 = outDir.x * outDir.x + outDir.y * outDir.y + outDir.z * outDir.z;
    if (len2 <= 1.0e-8f)
    {
        return false;
    }

    return true;
}

bool pointInQuad2D(float px, float py,
                   float x0, float y0,
                   float ux, float uy,
                   float vx, float vy,
                   float& outU, float& outV)
{
    const float wx = px - x0;
    const float wy = py - y0;
    const float denom = ux * vy - uy * vx;
    if (std::fabs(denom) <= 1.0e-8f)
    {
        return false;
    }
    outU = (wx * vy - wy * vx) / denom;
    outV = (ux * wy - uy * wx) / denom;
    return outU >= 0.0f && outU <= 1.0f && outV >= 0.0f && outV <= 1.0f;
}

} // namespace

void TerrainRenderer::setOverlayRects(const std::vector<OverlayRect>& rects)
{
    m_overlayRectsScreen = rects;
    if (m_overlayRectsScreen.size() > std::numeric_limits<uint16_t>::max())
    {
        m_overlayRectsScreen.resize(std::numeric_limits<uint16_t>::max());
    }
    m_overlayRectsWorld.clear();
    m_overlayWorldDirty = true;
    m_rectComputeDirty = true;
    if (m_rectMaxReadPending)
    {
        m_rectMaxReadCancelPending = true;
    }
    else
    {
        m_rectMaxHeights.clear();
    }
    m_rectMaxReadRequested = true;
}

void TerrainRenderer::clearOverlayRects()
{
    m_overlayRectsScreen.clear();
    m_overlayRectsWorld.clear();
    m_overlayWorldDirty = false;
    m_rectComputeDirty = true;
    if (m_rectMaxReadPending)
    {
        m_rectMaxReadCancelPending = true;
    }
    else
    {
        m_rectMaxHeights.clear();
        m_rectMaxReadRequested = false;
    }
}

void TerrainRenderer::setOverlayUseScreenSpace(bool enabled)
{
    if (m_overlayUseScreenSpace == enabled)
    {
        return;
    }

    m_overlayUseScreenSpace = enabled;
    m_overlayWorldDirty = true;
    m_rectComputeDirty = true;
}

void TerrainRenderer::setOverlayPixelScale(float scale)
{
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }

    if (std::fabs(m_overlayPixelScale - scale) < 0.0001f)
    {
        return;
    }

    m_overlayPixelScale = scale;
    m_overlayWorldDirty = true;
    m_rectComputeDirty = true;
}

void TerrainRenderer::requestOverlayMaxReadback()
{
    if (m_overlayRectsScreen.empty())
    {
        return;
    }
    m_rectMaxReadRequested = true;
}

bool TerrainRenderer::processOverlayMaxReadback(uint32_t frameId)
{
    if (!m_rectMaxReadPending)
    {
        return false;
    }

    if (frameId < m_rectMaxReadFrame)
    {
        return false;
    }

    if (m_rectMaxReadCancelPending)
    {
        m_rectMaxHeights.clear();
        m_rectMaxReadPending = false;
        m_rectMaxReadCancelPending = false;
        return true;
    }

    if (m_rectMaxReadCount == 0 || m_rectMaxReadback.empty())
    {
        m_rectMaxHeights.clear();
        m_rectMaxReadPending = false;
        return true;
    }

    const size_t count = std::min<size_t>(m_rectMaxReadCount, m_rectMaxReadback.size());
    m_rectMaxHeights.assign(m_rectMaxReadback.begin(), m_rectMaxReadback.begin() + count);
    m_rectMaxReadPending = false;
    return true;
}

bool TerrainRenderer::overlayMaxReady() const
{
    const size_t rectCount = m_overlayRectsWorld.size();
    return rectCount > 0 && m_rectMaxHeights.size() >= rectCount;
}

bool TerrainRenderer::getOverlayRectWorldBounds(int rectId,
                                                  float& outCenterX,
                                                  float& outCenterY,
                                                  float& outCenterZ,
                                                  float& outWidth,
                                                  float& outHeight,
                                                  float& outNormalX,
                                                  float& outNormalY,
                                                  float& outNormalZ) const
{
    const size_t worldCount = m_overlayRectsWorld.size();
    const size_t screenCount = m_overlayRectsScreen.size();
    for (const auto& rect : m_overlayRectsWorld)
    {
        if (rect.id != rectId)
            continue;

        const float ux = rect.ux;
        const float uy = rect.uy;
        const float vx = rect.vx;
        const float vy = rect.vy;
        outWidth = std::sqrt(ux * ux + uy * uy);
        outHeight = std::sqrt(vx * vx + vy * vy);
        outCenterX = rect.x + 0.5f * (ux + vx);
        outCenterY = rect.y + 0.5f * (uy + vy);
        outCenterZ = 0.0f;

        const float nx = uy * 0.0f - 0.0f * vy;
        const float ny = 0.0f * vx - ux * 0.0f;
        const float nz = ux * vy - uy * vx;
        const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nlen > 1.0e-6f)
        {
            outNormalX = nx / nlen;
            outNormalY = ny / nlen;
            outNormalZ = nz / nlen;
        }
        else
        {
            outNormalX = 0.0f;
            outNormalY = 0.0f;
            outNormalZ = 1.0f;
        }
        return true;
    }

    if (m_overlayUseScreenSpace)
    {
        LOG_D("[TerrainRenderer] Focus rect id={} not found (screen space, world={}, screen={})",
              rectId, worldCount, screenCount);
        return false;
    }
    if (m_heightfieldWidth == 0 || m_heightfieldHeight == 0)
    {
        LOG_D("[TerrainRenderer] Focus rect id={} blocked (heightfield not ready)", rectId);
        return false;
    }

    const float pixelW = float(m_heightfieldWidth);
    const float pixelH = float(m_heightfieldHeight);
    const float invW = 1.0f / pixelW;
    const float invH = 1.0f / pixelH;

    float model[16];
    buildModelMatrix(model);
    auto transformNormal = [&](float x, float y, float z, float& ox, float& oy, float& oz) {
        ox = model[0] * x + model[4] * y + model[8] * z;
        oy = model[1] * x + model[5] * y + model[9] * z;
        oz = model[2] * x + model[6] * y + model[10] * z;
    };

    auto transformPoint = [&](float x, float y, float z, float& ox, float& oy, float& oz) {
        ox = model[0] * x + model[4] * y + model[8] * z + model[12];
        oy = model[1] * x + model[5] * y + model[9] * z + model[13];
        oz = model[2] * x + model[6] * y + model[10] * z + model[14];
    };

    for (const auto& rect : m_overlayRectsScreen)
    {
        if (rect.id != rectId)
            continue;

        float x0 = rect.x;
        float y0 = rect.y;
        float x1 = rect.x + rect.width;
        float y1 = rect.y + rect.height;

        if (rect.coordType == OverlayCoordType::NormalizedCenter)
        {
            const float centerX = rect.x * pixelW;
            const float centerY = rect.y * pixelH;
            const float rectW = rect.width * pixelW;
            const float rectH = rect.height * pixelH;
            x0 = centerX - rectW * 0.5f;
            y0 = centerY - rectH * 0.5f;
            x1 = centerX + rectW * 0.5f;
            y1 = centerY + rectH * 0.5f;
        }
        else if (rect.coordType == OverlayCoordType::PixelCenter)
        {
            const float baseW = rect.imageWidth > 0.0f ? rect.imageWidth : pixelW;
            const float baseH = rect.imageHeight > 0.0f ? rect.imageHeight : pixelH;
            if (baseW > 0.0f && baseH > 0.0f)
            {
                const float centerX = (rect.x / baseW + 0.5f) * pixelW;
                const float centerY = (rect.y / baseH + 0.5f) * pixelH;
                const float rectW = (rect.width / baseW) * pixelW;
                const float rectH = (rect.height / baseH) * pixelH;
                x0 = centerX - rectW * 0.5f;
                y0 = centerY - rectH * 0.5f;
                x1 = centerX + rectW * 0.5f;
                y1 = centerY + rectH * 0.5f;
            }
        }

        const float centerX = (x0 + x1) * 0.5f;
        const float centerY = (y0 + y1) * 0.5f;
        const float rectW = std::fabs(x1 - x0);
        const float rectH = std::fabs(y1 - y0);
        if (rectW <= 1.0e-6f || rectH <= 1.0e-6f)
            return false;

        const float centerU = centerX * invW;
        const float centerV = centerY * invH;
        const float localX = (centerU * 2.0f - 1.0f) * m_terrainAspectRatio;
        const float localY = (centerV * 2.0f - 1.0f);
        const float localW = rectW * invW * 2.0f * m_terrainAspectRatio;
        const float localH = rectH * invH * 2.0f;

        float cx = 0.0f;
        float cy = 0.0f;
        float cz = 0.0f;
        transformPoint(localX, localY, 0.0f, cx, cy, cz);

        float wx1 = 0.0f;
        float wy1 = 0.0f;
        float wz1 = 0.0f;
        float wx2 = 0.0f;
        float wy2 = 0.0f;
        float wz2 = 0.0f;
        float hx1 = 0.0f;
        float hy1 = 0.0f;
        float hz1 = 0.0f;
        float hx2 = 0.0f;
        float hy2 = 0.0f;
        float hz2 = 0.0f;

        transformPoint(localX + localW * 0.5f, localY, 0.0f, wx1, wy1, wz1);
        transformPoint(localX - localW * 0.5f, localY, 0.0f, wx2, wy2, wz2);
        transformPoint(localX, localY + localH * 0.5f, 0.0f, hx1, hy1, hz1);
        transformPoint(localX, localY - localH * 0.5f, 0.0f, hx2, hy2, hz2);

        const float wdx = wx1 - wx2;
        const float wdy = wy1 - wy2;
        const float wdz = wz1 - wz2;
        const float hdx = hx1 - hx2;
        const float hdy = hy1 - hy2;
        const float hdz = hz1 - hz2;

        float nnx = 0.0f;
        float nny = 0.0f;
        float nnz = 0.0f;
        transformNormal(0.0f, 0.0f, 1.0f, nnx, nny, nnz);
        const float nlen = std::sqrt(nnx * nnx + nny * nny + nnz * nnz);
        if (nlen > 1.0e-6f)
        {
            outNormalX = nnx / nlen;
            outNormalY = nny / nlen;
            outNormalZ = nnz / nlen;
        }
        else
        {
            outNormalX = 0.0f;
            outNormalY = 0.0f;
            outNormalZ = 1.0f;
        }

        outCenterX = cx;
        outCenterY = cy;
        outCenterZ = cz;
        outWidth = std::sqrt(wdx * wdx + wdy * wdy + wdz * wdz);
        outHeight = std::sqrt(hdx * hdx + hdy * hdy + hdz * hdz);
        return true;
    }

    LOG_D("[TerrainRenderer] Focus rect id={} not found (world={}, screen={})",
          rectId, worldCount, screenCount);
    return false;
}

bool TerrainRenderer::getOverlayRectNearestEdgeTargetYaw(int rectId, float& outYawDeg) const
{
    const float pixelW = float(m_heightfieldWidth);
    const float pixelH = float(m_heightfieldHeight);
    if (pixelW <= 0.0f || pixelH <= 0.0f)
    {
        return false;
    }

    for (const auto& rect : m_overlayRectsScreen)
    {
        if (rect.id != rectId)
        {
            continue;
        }

        float x0 = rect.x;
        float y0 = rect.y;
        float x1 = rect.x + rect.width;
        float y1 = rect.y + rect.height;

        if (rect.coordType == OverlayCoordType::NormalizedCenter)
        {
            const float centerX = rect.x * pixelW;
            const float centerY = rect.y * pixelH;
            const float rectW = rect.width * pixelW;
            const float rectH = rect.height * pixelH;
            x0 = centerX - rectW * 0.5f;
            y0 = centerY - rectH * 0.5f;
            x1 = centerX + rectW * 0.5f;
            y1 = centerY + rectH * 0.5f;
        }
        else if (rect.coordType == OverlayCoordType::PixelCenter)
        {
            const float baseW = rect.imageWidth > 0.0f ? rect.imageWidth : pixelW;
            const float baseH = rect.imageHeight > 0.0f ? rect.imageHeight : pixelH;
            if (baseW > 0.0f && baseH > 0.0f)
            {
                const float centerX = (rect.x / baseW + 0.5f) * pixelW;
                const float centerY = (rect.y / baseH + 0.5f) * pixelH;
                const float rectW = (rect.width / baseW) * pixelW;
                const float rectH = (rect.height / baseH) * pixelH;
                x0 = centerX - rectW * 0.5f;
                y0 = centerY - rectH * 0.5f;
                x1 = centerX + rectW * 0.5f;
                y1 = centerY + rectH * 0.5f;
            }
        }

        const float minX = std::min(x0, x1);
        const float maxX = std::max(x0, x1);
        const float minY = std::min(y0, y1);
        const float maxY = std::max(y0, y1);
        const float distLeft = minX;
        const float distRight = pixelW - maxX;
        const float distTop = minY;
        const float distBottom = pixelH - maxY;

        float localNx = -1.0f;
        float localNy = 0.0f;
        float minDist = distLeft;

        if (distRight < minDist)
        {
            minDist = distRight;
            localNx = 1.0f;
            localNy = 0.0f;
        }
        if (distTop < minDist)
        {
            minDist = distTop;
            localNx = 0.0f;
            localNy = -1.0f;
        }
        if (distBottom < minDist)
        {
            localNx = 0.0f;
            localNy = 1.0f;
        }

        const float rotRad = bx::toRad(m_imageRotation);
        const float c = std::cos(rotRad);
        const float s = std::sin(rotRad);

        const float nxRot = c * localNx - s * localNy;
        const float nyRot = s * localNx + c * localNy;

        // Local XY plane becomes world XZ plane after the renderer's -90deg X tilt.
        const float normalWorldX = nxRot;
        const float normalWorldZ = -nyRot;

        // Camera forward should point opposite to the outward side normal.
        const float desiredForwardX = -normalWorldX;
        const float desiredForwardZ = -normalWorldZ;
        outYawDeg = bx::toDeg(std::atan2(desiredForwardX, desiredForwardZ));
        return true;
    }

    return false;
}

bool TerrainRenderer::getAlgorithmDenseSideTargetYaw(float& outYawDeg, int& outRectId) const
{
    outYawDeg = 0.0f;
    outRectId = -1;

    const float pixelW = float(m_heightfieldWidth);
    const float pixelH = float(m_heightfieldHeight);
    if (pixelW <= 0.0f || pixelH <= 0.0f)
    {
        return false;
    }

    enum Side : int { Left = 0, Right = 1, Top = 2, Bottom = 3, SideCount = 4 };
    float sideScore[SideCount] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float sideBestDist[SideCount] = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
    int sideBestRectId[SideCount] = { -1, -1, -1, -1 };

    int algorithmRectCount = 0;
    for (const auto& rect : m_overlayRectsScreen)
    {
        // Algorithm rect IDs are stable negative values in NGViewModel.
        if (rect.id >= 0)
        {
            continue;
        }

        float x0 = rect.x;
        float y0 = rect.y;
        float x1 = rect.x + rect.width;
        float y1 = rect.y + rect.height;

        if (rect.coordType == OverlayCoordType::NormalizedCenter)
        {
            const float centerX = rect.x * pixelW;
            const float centerY = rect.y * pixelH;
            const float rectW = rect.width * pixelW;
            const float rectH = rect.height * pixelH;
            x0 = centerX - rectW * 0.5f;
            y0 = centerY - rectH * 0.5f;
            x1 = centerX + rectW * 0.5f;
            y1 = centerY + rectH * 0.5f;
        }
        else if (rect.coordType == OverlayCoordType::PixelCenter)
        {
            const float baseW = rect.imageWidth > 0.0f ? rect.imageWidth : pixelW;
            const float baseH = rect.imageHeight > 0.0f ? rect.imageHeight : pixelH;
            if (baseW > 0.0f && baseH > 0.0f)
            {
                const float centerX = (rect.x / baseW + 0.5f) * pixelW;
                const float centerY = (rect.y / baseH + 0.5f) * pixelH;
                const float rectW = (rect.width / baseW) * pixelW;
                const float rectH = (rect.height / baseH) * pixelH;
                x0 = centerX - rectW * 0.5f;
                y0 = centerY - rectH * 0.5f;
                x1 = centerX + rectW * 0.5f;
                y1 = centerY + rectH * 0.5f;
            }
        }

        const float minX = std::min(x0, x1);
        const float maxX = std::max(x0, x1);
        const float minY = std::min(y0, y1);
        const float maxY = std::max(y0, y1);
        const float rectW = std::fabs(maxX - minX);
        const float rectH = std::fabs(maxY - minY);
        if (rectW <= 1.0e-6f || rectH <= 1.0e-6f)
        {
            continue;
        }

        ++algorithmRectCount;

        const float distLeft = minX;
        const float distRight = pixelW - maxX;
        const float distTop = minY;
        const float distBottom = pixelH - maxY;

        Side nearestSide = Left;
        float nearestDist = distLeft;
        if (distRight < nearestDist) { nearestDist = distRight; nearestSide = Right; }
        if (distTop < nearestDist) { nearestDist = distTop; nearestSide = Top; }
        if (distBottom < nearestDist) { nearestDist = distBottom; nearestSide = Bottom; }

        // Density weight: closer to model edge => larger contribution.
        const float closeness = 1.0f / (nearestDist + 1.0f);

        // Orientation weight: favor rectangles whose edge direction follows the side tangent.
        const float angleRad = bx::toRad(rect.angle);
        const float ux = std::cos(angleRad);
        const float uy = std::sin(angleRad);
        const float vx = -std::sin(angleRad);
        const float vy = std::cos(angleRad);

        float tangentAlign = 1.0f;
        if (nearestSide == Left || nearestSide == Right)
        {
            tangentAlign = std::max(std::fabs(uy), std::fabs(vy)); // vertical tangent
        }
        else
        {
            tangentAlign = std::max(std::fabs(ux), std::fabs(vx)); // horizontal tangent
        }

        const float score = closeness * (0.7f + 0.3f * tangentAlign);
        sideScore[int(nearestSide)] += score;

        if (nearestDist < sideBestDist[int(nearestSide)])
        {
            sideBestDist[int(nearestSide)] = nearestDist;
            sideBestRectId[int(nearestSide)] = rect.id;
        }
    }

    if (algorithmRectCount <= 0)
    {
        return false;
    }

    Side bestSide = Left;
    float bestScore = sideScore[int(bestSide)];
    for (int i = 1; i < SideCount; ++i)
    {
        if (sideScore[i] > bestScore)
        {
            bestScore = sideScore[i];
            bestSide = Side(i);
        }
    }

    if (bestScore <= 0.0f)
    {
        return false;
    }

    float localNx = -1.0f;
    float localNy = 0.0f;
    switch (bestSide)
    {
    case Left:   localNx = -1.0f; localNy =  0.0f; break;
    case Right:  localNx =  1.0f; localNy =  0.0f; break;
    case Top:    localNx =  0.0f; localNy = -1.0f; break;
    case Bottom: localNx =  0.0f; localNy =  1.0f; break;
    default: break;
    }

    const float rotRad = bx::toRad(m_imageRotation);
    const float c = std::cos(rotRad);
    const float s = std::sin(rotRad);

    const float nxRot = c * localNx - s * localNy;
    const float nyRot = s * localNx + c * localNy;

    // Local XY plane becomes world XZ plane after renderer tilt.
    const float normalWorldX = nxRot;
    const float normalWorldZ = -nyRot;
    const float desiredForwardX = -normalWorldX;
    const float desiredForwardZ = -normalWorldZ;

    outYawDeg = bx::toDeg(std::atan2(desiredForwardX, desiredForwardZ));
    outRectId = sideBestRectId[int(bestSide)];

    LOG_I("[TerrainRenderer] Dense algorithm side target: side={}, score={:.5f}, rectId={}, yaw={:.3f}",
          int(bestSide), bestScore, outRectId, outYawDeg);
    return true;
}

bool TerrainRenderer::hasOverlayRects() const
{
    return !m_overlayRectsScreen.empty();
}

int TerrainRenderer::pickOverlayRect(float sx, float sy) const
{
    if (m_overlayUseScreenSpace)
    {
        return -1;
    }

    if (!m_hasViewProj || m_width == 0 || m_height == 0)
    {
        return -1;
    }

    if (m_overlayRectsWorld.empty() || m_rectMaxHeights.size() < m_overlayRectsWorld.size())
    {
        return -1;
    }

    const bgfx::Caps* caps = bgfx::getCaps();
    const float ndcNear = (caps && caps->homogeneousDepth) ? 0.0f : -1.0f;

    float viewProj[16];
    float invViewProj[16];
    float model[16];
    float invModel[16];
    bx::mtxMul(viewProj, m_viewMtx, m_projMtx);
    bx::mtxInverse(invViewProj, viewProj);
    buildModelMatrix(model);
    bx::mtxInverse(invModel, model);

    bx::Vec3 origin = { 0.0f, 0.0f, 0.0f };
    bx::Vec3 dir = { 0.0f, 0.0f, 0.0f };
    if (!screenToLocalRay(sx, sy, float(m_width), float(m_height), ndcNear, invViewProj, invModel, origin, dir))
    {
        return -1;
    }

    const float dirZ = dir.z;
    if (std::fabs(dirZ) <= 1.0e-6f)
    {
        return -1;
    }

    float bestT = std::numeric_limits<float>::max();
    int bestId = -1;

    const size_t rectCount = m_overlayRectsWorld.size();
    for (size_t i = 0; i < rectCount; ++i)
    {
        const OverlayQuad& rect = m_overlayRectsWorld[i];
        const float height = m_rectMaxHeights[i];
        if (!std::isfinite(height))
        {
            continue;
        }

        const float t = (height - origin.z) / dirZ;
        if (t < 0.0f || t >= bestT)
        {
            continue;
        }

        const bx::Vec3 hit = bx::add(origin, bx::mul(dir, t));
        float u = 0.0f;
        float v = 0.0f;
        if (!pointInQuad2D(hit.x, hit.y, rect.x, rect.y, rect.ux, rect.uy, rect.vx, rect.vy, u, v))
        {
            continue;
        }

        bestT = t;
        bestId = rect.id;
    }

    return bestId;
}

void TerrainRenderer::loadOverlayBuffers()
{
    if (bgfx::isValid(m_rectWireVertices) && bgfx::isValid(m_rectWireIndices))
    {
        return;
    }

    m_rectWireLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .end();

    m_rectParamLayout.begin()
        .add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float)
        .end();

    struct RectWireVertex
    {
        float edgeId;
        float along;
        float side;
    };

    RectWireVertex vertices[12 * 4];
    uint16_t indices[12 * 6];

    for (uint16_t edge = 0; edge < 12; ++edge)
    {
        const uint16_t base = edge * 4;
        vertices[base + 0] = { float(edge), 0.0f, -1.0f };
        vertices[base + 1] = { float(edge), 0.0f,  1.0f };
        vertices[base + 2] = { float(edge), 1.0f, -1.0f };
        vertices[base + 3] = { float(edge), 1.0f,  1.0f };

        const uint16_t i = edge * 6;
        indices[i + 0] = base + 0;
        indices[i + 1] = base + 1;
        indices[i + 2] = base + 2;
        indices[i + 3] = base + 1;
        indices[i + 4] = base + 3;
        indices[i + 5] = base + 2;
    }

    m_rectWireVertices = bgfx::createVertexBuffer(
        bgfx::copy(vertices, sizeof(vertices)),
        m_rectWireLayout
    );

    m_rectWireIndices = bgfx::createIndexBuffer(
        bgfx::copy(indices, sizeof(indices))
    );
}

void TerrainRenderer::updateOverlayGpuData()
{
    const bool noCompute = RenderDevice::renderCaps().noCompute();
    const bgfx::Caps* rectCaps = bgfx::getCaps();
    bool rectComputeAvailable = !noCompute
        && rectCaps
        && (rectCaps->supported & BGFX_CAPS_COMPUTE) != 0
        && bgfx::isValid(m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION])
        && bgfx::isValid(m_rectParamsHandle)
        && bgfx::isValid(m_rectSampleParamsHandle);

    if (rectComputeAvailable
        && !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::R32F, BGFX_TEXTURE_COMPUTE_WRITE))
    {
        rectComputeAvailable = false;
    }

    const bool wantReadback = rectComputeAvailable
        && m_rectMaxReadRequested
        && !m_rectMaxReadPending;
    if (m_rectMaxReadPending)
    {
        return;
    }
    if (!m_rectComputeDirty && !m_overlayWorldDirty)
    {
        if (wantReadback && !m_overlayRectsWorld.empty()
            && bgfx::isValid(m_rectMaxTexture) && bgfx::isValid(m_rectMaxReadTexture)
            && m_rectMaxTextureWidth > 0)
        {
            m_rectMaxReadback.resize(m_rectMaxTextureWidth);
            bgfx::blit(m_viewId, m_rectMaxReadTexture, 0, 0, m_rectMaxTexture);
            const uint32_t frameId = bgfx::readTexture(
                m_rectMaxReadTexture,
                m_rectMaxReadback.data());
            if (frameId != std::numeric_limits<uint32_t>::max())
            {
                m_rectMaxReadFrame = frameId;
                m_rectMaxReadCount = uint16_t(m_overlayRectsWorld.size());
                m_rectMaxReadPending = true;
                m_rectMaxReadRequested = false;
                m_rectMaxReadSubmitFrame = terrain_internal::currentFrameId();
            }
        }
        return;
    }

    if (m_overlayRectsScreen.empty())
    {
        m_overlayRectsWorld.clear();
        m_overlayWorldDirty = false;
        m_rectComputeDirty = false;
        if (!m_rectMaxReadPending)
        {
            m_rectMaxHeights.clear();
            m_rectMaxReadRequested = false;
        }
        return;
    }

    if (!m_heightfieldReady)
    {
        return;
    }

    if (!bgfx::isValid(m_textures[types::TEXTURE_DMAP]))
    {
        return;
    }

    if (m_overlayWorldDirty)
    {
        float viewW = 0.0f;
        float viewH = 0.0f;
        float invViewProj[16];
        float invModel[16];
        float ndcNear = 0.0f;

        if (m_overlayUseScreenSpace)
        {
            if (!m_hasViewProj)
            {
                return;
            }

            viewW = float(m_width);
            viewH = float(m_height);
            if (viewW <= 0.0f || viewH <= 0.0f)
            {
                return;
            }

            float viewProj[16];
            bx::mtxMul(viewProj, m_viewMtx, m_projMtx);
            bx::mtxInverse(invViewProj, viewProj);

            float model[16];
            buildModelMatrix(model);
            bx::mtxInverse(invModel, model);

            const bgfx::Caps* caps = bgfx::getCaps();
            ndcNear = (caps && caps->homogeneousDepth) ? 0.0f : -1.0f;
        }

        m_overlayRectsWorld.clear();
        m_overlayRectsWorld.reserve(m_overlayRectsScreen.size());

        for (const OverlayRect& rect : m_overlayRectsScreen)
        {
            bx::Vec3 p00 = { 0.0f, 0.0f, 0.0f };
            bx::Vec3 p10 = { 0.0f, 0.0f, 0.0f };
            bx::Vec3 p01 = { 0.0f, 0.0f, 0.0f };

            if (m_overlayUseScreenSpace)
            {
                const float sx0 = rect.x * m_overlayPixelScale;
                const float sy0 = rect.y * m_overlayPixelScale;
                const float sx1 = (rect.x + rect.width) * m_overlayPixelScale;
                const float sy1 = (rect.y + rect.height) * m_overlayPixelScale;

                const float minSx = std::min(sx0, sx1);
                const float maxSx = std::max(sx0, sx1);
                const float minSy = std::min(sy0, sy1);
                const float maxSy = std::max(sy0, sy1);

                bx::Vec3 p11 = { 0.0f, 0.0f, 0.0f };
                if (!screenToLocalPoint(minSx, minSy, viewW, viewH, ndcNear, invViewProj, invModel, p00) ||
                    !screenToLocalPoint(maxSx, minSy, viewW, viewH, ndcNear, invViewProj, invModel, p10) ||
                    !screenToLocalPoint(maxSx, maxSy, viewW, viewH, ndcNear, invViewProj, invModel, p11) ||
                    !screenToLocalPoint(minSx, maxSy, viewW, viewH, ndcNear, invViewProj, invModel, p01))
                {
                    continue;
                }

                const float minX = std::min(std::min(p00.x, p10.x), std::min(p11.x, p01.x));
                const float maxX = std::max(std::max(p00.x, p10.x), std::max(p11.x, p01.x));
                const float minY = std::min(std::min(p00.y, p10.y), std::min(p11.y, p01.y));
                const float maxY = std::max(std::max(p00.y, p10.y), std::max(p11.y, p01.y));

                if (maxX <= minX || maxY <= minY)
                {
                    continue;
                }

                p00 = { minX, minY, 0.0f };
                p10 = { maxX, minY, 0.0f };
                p01 = { minX, maxY, 0.0f };
            }
            else
            {
                if (m_heightfieldWidth == 0 || m_heightfieldHeight == 0)
                {
                    continue;
                }

                const float pixelW = float(m_heightfieldWidth);
                const float pixelH = float(m_heightfieldHeight);
                const float invW = 1.0f / pixelW;
                const float invH = 1.0f / pixelH;

                float x0 = rect.x;
                float y0 = rect.y;
                float x1 = rect.x + rect.width;
                float y1 = rect.y + rect.height;

                if (rect.coordType == OverlayCoordType::NormalizedCenter)
                {
                    const float centerX = rect.x * pixelW;
                    const float centerY = rect.y * pixelH;
                    const float rectW = rect.width * pixelW;
                    const float rectH = rect.height * pixelH;
                    x0 = centerX - rectW * 0.5f;
                    y0 = centerY - rectH * 0.5f;
                    x1 = centerX + rectW * 0.5f;
                    y1 = centerY + rectH * 0.5f;
                }
                else if (rect.coordType == OverlayCoordType::PixelCenter)
                {
                    const float baseW = rect.imageWidth > 0.0f ? rect.imageWidth : pixelW;
                    const float baseH = rect.imageHeight > 0.0f ? rect.imageHeight : pixelH;
                    if (baseW > 0.0f && baseH > 0.0f)
                    {
                        const float centerX = (rect.x / baseW + 0.5f) * pixelW;
                        const float centerY = (rect.y / baseH + 0.5f) * pixelH;
                        const float rectW = (rect.width / baseW) * pixelW;
                        const float rectH = (rect.height / baseH) * pixelH;
                        x0 = centerX - rectW * 0.5f;
                        y0 = centerY - rectH * 0.5f;
                        x1 = centerX + rectW * 0.5f;
                        y1 = centerY + rectH * 0.5f;
                    }
                }

                const float centerX = (x0 + x1) * 0.5f;
                const float centerY = (y0 + y1) * 0.5f;
                const float rectW = std::fabs(x1 - x0);
                const float rectH = std::fabs(y1 - y0);
                if (rectW <= 1.0e-6f || rectH <= 1.0e-6f)
                {
                    continue;
                }

                const float centerU = centerX * invW;
                const float centerV = centerY * invH;
                const float worldCx = (centerU * 2.0f - 1.0f) * m_terrainAspectRatio;
                const float worldCy = (centerV * 2.0f - 1.0f);

                const float worldW = rectW * invW * 2.0f * m_terrainAspectRatio;
                const float worldH = rectH * invH * 2.0f;

                float uVecX = worldW;
                float uVecY = 0.0f;
                float vVecX = 0.0f;
                float vVecY = worldH;

                if (std::fabs(rect.angle) > 0.0001f)
                {
                    const float angleRad = bx::toRad(rect.angle);
                    const float c = std::cos(angleRad);
                    const float s = std::sin(angleRad);
                    const float ruX = uVecX * c - uVecY * s;
                    const float ruY = uVecX * s + uVecY * c;
                    const float rvX = vVecX * c - vVecY * s;
                    const float rvY = vVecX * s + vVecY * c;
                    uVecX = ruX;
                    uVecY = ruY;
                    vVecX = rvX;
                    vVecY = rvY;
                }

                const float halfUx = uVecX * 0.5f;
                const float halfUy = uVecY * 0.5f;
                const float halfVx = vVecX * 0.5f;
                const float halfVy = vVecY * 0.5f;

                p00 = { worldCx - halfUx - halfVx, worldCy - halfUy - halfVy, 0.0f };
                p10 = { p00.x + uVecX, p00.y + uVecY, 0.0f };
                p01 = { p00.x + vVecX, p00.y + vVecY, 0.0f };
            }

            const bx::Vec3 uVec = bx::sub(p10, p00);
            const bx::Vec3 vVec = bx::sub(p01, p00);
            const float uLen2 = uVec.x * uVec.x + uVec.y * uVec.y;
            const float vLen2 = vVec.x * vVec.x + vVec.y * vVec.y;
            if (uLen2 <= 1.0e-6f || vLen2 <= 1.0e-6f)
            {
                continue;
            }

            OverlayQuad worldRect{};
            worldRect.id = rect.id;
            worldRect.x = p00.x;
            worldRect.y = p00.y;
            worldRect.ux = uVec.x;
            worldRect.uy = uVec.y;
            worldRect.vx = vVec.x;
            worldRect.vy = vVec.y;
            worldRect.color[0] = rect.color[0];
            worldRect.color[1] = rect.color[1];
            worldRect.color[2] = rect.color[2];
            worldRect.color[3] = rect.color[3];
            worldRect.lineWidth = rect.lineWidth;
            worldRect.dashLength = rect.dashLength;
            worldRect.dashGap = rect.dashGap;
            worldRect.blinkPeriod = rect.blinkPeriod;
            worldRect.blinkDuty = rect.blinkDuty;
            m_overlayRectsWorld.push_back(worldRect);
        }

        m_overlayWorldDirty = false;
        m_rectComputeDirty = true;
    }

    if (m_overlayRectsWorld.empty())
    {
        m_rectComputeDirty = false;
        return;
    }

    if (!m_rectComputeDirty)
    {
        return;
    }

    const uint16_t rectCount = uint16_t(m_overlayRectsWorld.size());
    if (!ensureOverlayMaxTexture(rectCount, rectComputeAvailable, rectComputeAvailable))
    {
        if (rectComputeAvailable)
        {
            rectComputeAvailable = false;
            if (!ensureOverlayMaxTexture(rectCount, false, false))
            {
                return;
            }
        }
        else
        {
            return;
        }
    }

    if (!rectComputeAvailable)
    {
        m_rectMaxHeights.assign(m_rectMaxTextureWidth, 0.0f);
        if (!m_heightfieldCpu.empty()
            && m_heightfieldCpuWidth == m_heightfieldWidth
            && m_heightfieldCpuHeight == m_heightfieldHeight
            && m_heightfieldCpuWidth > 0
            && m_heightfieldCpuHeight > 0)
        {
            constexpr int kSampleGrid = 16;
            const float invScale = 1.0f / 65535.0f;
            const float halfW = m_terrainAspectRatio;
            const float halfH = 1.0f;
            for (uint16_t i = 0; i < rectCount; ++i)
            {
                const OverlayQuad& rect = m_overlayRectsWorld[i];
                float maxHeight = 0.0f;
                for (int gy = 0; gy < kSampleGrid; ++gy)
                {
                    for (int gx = 0; gx < kSampleGrid; ++gx)
                    {
                        const float tx = (float(gx) + 0.5f) / float(kSampleGrid);
                        const float ty = (float(gy) + 0.5f) / float(kSampleGrid);
                        const float posX = rect.x + rect.ux * tx + rect.vx * ty;
                        const float posY = rect.y + rect.uy * tx + rect.vy * ty;
                        float u = (posX + halfW) / (2.0f * halfW);
                        float v = (posY + halfH) / (2.0f * halfH);
                        u = std::min(1.0f, std::max(0.0f, u));
                        v = std::min(1.0f, std::max(0.0f, v));
                        const int ix = std::min<int>(int(u * m_heightfieldCpuWidth), m_heightfieldCpuWidth - 1);
                        const int iy = std::min<int>(int(v * m_heightfieldCpuHeight), m_heightfieldCpuHeight - 1);
                        const size_t idx = size_t(iy) * m_heightfieldCpuWidth + size_t(ix);
                        const float h = float(m_heightfieldCpu[idx]) * invScale * currentRenderDmapFactor() + currentRenderDmapBias();
                        if (h > maxHeight)
                        {
                            maxHeight = h;
                        }
                    }
                }
                m_rectMaxHeights[i] = maxHeight;
            }
        }
        const bgfx::Memory* rectMaxMem = bgfx::copy(
            m_rectMaxHeights.data(),
            uint32_t(m_rectMaxTextureWidth * sizeof(float))
        );
        bgfx::updateTexture2D(m_rectMaxTexture, 0, 0, 0, 0,
                              m_rectMaxTextureWidth, 1, rectMaxMem);
        m_rectComputeDirty = false;
        m_rectMaxReadRequested = false;
        m_rectMaxReadPending = false;
        return;
    }

    if (!ensureOverlayRectBuffers(rectCount))
    {
        return;
    }

    struct RectGpu
    {
        float p0x;
        float p0y;
        float ux;
        float uy;
        float vx;
        float vy;
        float pad0;
        float pad1;
    };

    std::vector<RectGpu> rects(rectCount);
    for (uint16_t i = 0; i < rectCount; ++i)
    {
        rects[i].p0x = m_overlayRectsWorld[i].x;
        rects[i].p0y = m_overlayRectsWorld[i].y;
        rects[i].ux = m_overlayRectsWorld[i].ux;
        rects[i].uy = m_overlayRectsWorld[i].uy;
        rects[i].vx = m_overlayRectsWorld[i].vx;
        rects[i].vy = m_overlayRectsWorld[i].vy;
        rects[i].pad0 = 0.0f;
        rects[i].pad1 = 0.0f;
    }

    const bgfx::Memory* rectMem = bgfx::copy(rects.data(), uint32_t(rects.size() * sizeof(RectGpu)));
    bgfx::update(m_rectParamsBuffer, 0, rectMem);

    const float rectParams[4] = { float(rectCount), 0.0f, 0.0f, 0.0f };
    const float sampleParams[4] = { m_terrainAspectRatio, 1.0f, currentRenderDmapFactor(), currentRenderDmapBias() };

    bgfx::setUniform(m_rectParamsHandle, rectParams);
    bgfx::setUniform(m_rectSampleParamsHandle, sampleParams);
    bgfx::setBuffer(0, m_rectParamsBuffer, bgfx::Access::Read);
    bgfx::setTexture(1, m_samplers[types::TERRAIN_DMAP_SAMPLER],
        m_textures[types::TEXTURE_DMAP],
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);
    bgfx::setImage(2, m_rectMaxTexture, 0, bgfx::Access::Write, bgfx::TextureFormat::R32F);
    bgfx::dispatch(m_viewId, m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION], rectCount, 1, 1);

    m_rectComputeDirty = false;

    if (wantReadback && bgfx::isValid(m_rectMaxTexture) && bgfx::isValid(m_rectMaxReadTexture)
        && m_rectMaxTextureWidth > 0)
    {
        m_rectMaxReadback.resize(m_rectMaxTextureWidth);
        bgfx::blit(m_viewId, m_rectMaxReadTexture, 0, 0, m_rectMaxTexture);
        const uint32_t frameId = bgfx::readTexture(
            m_rectMaxReadTexture,
            m_rectMaxReadback.data());
        if (frameId != std::numeric_limits<uint32_t>::max())
        {
            m_rectMaxReadFrame = frameId;
            m_rectMaxReadCount = rectCount;
            m_rectMaxReadPending = true;
            m_rectMaxReadRequested = false;
            m_rectMaxReadSubmitFrame = terrain_internal::currentFrameId();
        }
    }
}

bool TerrainRenderer::ensureOverlayRectBuffers(uint16_t rectCount)
{
    if (rectCount == 0)
    {
        return false;
    }

    const uint16_t requiredEntries = uint16_t(rectCount * 2);
    if (!bgfx::isValid(m_rectParamsBuffer) || m_rectBufferCapacity < requiredEntries)
    {
        if (bgfx::isValid(m_rectParamsBuffer))
        {
            bgfx::destroy(m_rectParamsBuffer);
        }

        m_rectBufferCapacity = nextPow2(requiredEntries);
        m_rectParamsBuffer = bgfx::createDynamicVertexBuffer(
            m_rectBufferCapacity,
            m_rectParamLayout,
            BGFX_BUFFER_COMPUTE_READ
        );
    }

    return bgfx::isValid(m_rectParamsBuffer);
}

bool TerrainRenderer::ensureOverlayMaxTexture(uint16_t rectCount, bool useCompute, bool needReadback)
{
    if (rectCount == 0)
    {
        return false;
    }

    const bool needRecreate = !bgfx::isValid(m_rectMaxTexture)
        || m_rectMaxTextureWidth < rectCount
        || m_rectMaxTextureCompute != useCompute;

    if (needRecreate)
    {
        if (bgfx::isValid(m_rectMaxTexture))
        {
            bgfx::destroy(m_rectMaxTexture);
        }

        m_rectMaxTextureWidth = nextPow2(rectCount);
        uint64_t texFlags = BGFX_TEXTURE_NONE
            | BGFX_SAMPLER_U_CLAMP
            | BGFX_SAMPLER_V_CLAMP
            | BGFX_SAMPLER_MIN_POINT
            | BGFX_SAMPLER_MAG_POINT;
        if (useCompute)
        {
            texFlags |= BGFX_TEXTURE_COMPUTE_WRITE;
        }

        m_rectMaxTexture = bgfx::createTexture2D(
            m_rectMaxTextureWidth,
            1,
            false,
            1,
            bgfx::TextureFormat::R32F,
            texFlags
        );
        m_rectMaxTextureCompute = useCompute;
    }

    if (needReadback && m_rectMaxTextureWidth > 0)
    {
        if (!bgfx::isValid(m_rectMaxReadTexture) || m_rectMaxTextureWidth < rectCount)
        {
            if (bgfx::isValid(m_rectMaxReadTexture))
            {
                bgfx::destroy(m_rectMaxReadTexture);
            }
            const uint64_t readFlags = BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST;
            m_rectMaxReadTexture = bgfx::createTexture2D(
                m_rectMaxTextureWidth,
                1,
                false,
                1,
                bgfx::TextureFormat::R32F,
                readFlags
            );
        }
    }

    if (needReadback)
    {
        return bgfx::isValid(m_rectMaxTexture) && bgfx::isValid(m_rectMaxReadTexture);
    }
    return bgfx::isValid(m_rectMaxTexture);
}

void TerrainRenderer::renderOverlayRects()
{
    if (!m_heightfieldReady)
    {
        return;
    }

    if (m_overlayRectsWorld.empty())
    {
        return;
    }

    if (!bgfx::isValid(m_programRectWire) ||
        !bgfx::isValid(m_rectWireVertices) ||
        !bgfx::isValid(m_rectWireIndices) ||
        !bgfx::isValid(m_rectMaxTexture) ||
        !bgfx::isValid(m_rectMaxSampler) ||
        !bgfx::isValid(m_rectMaxParamsHandle) ||
        !bgfx::isValid(m_rectViewParamsHandle) ||
        !bgfx::isValid(m_rectDebugParamsHandle))
    {
        return;
    }

    const uint32_t rectCount = uint32_t(m_overlayRectsWorld.size());
    const uint16_t instanceStride = sizeof(float) * 16;
    const uint32_t avail = bgfx::getAvailInstanceDataBuffer(rectCount, instanceStride);
    if (avail == 0)
    {
        return;
    }

    const uint32_t drawCount = std::min(rectCount, avail);
    bgfx::InstanceDataBuffer idb;
    bgfx::allocInstanceDataBuffer(&idb, drawCount, instanceStride);

    uint8_t* data = idb.data;
    for (uint32_t i = 0; i < drawCount; ++i)
    {
        const OverlayQuad& rect = m_overlayRectsWorld[i];
        float* dst = reinterpret_cast<float*>(data);

        dst[0] = rect.x;
        dst[1] = rect.y;
        dst[2] = rect.ux;
        dst[3] = rect.uy;

        dst[4] = rect.vx;
        dst[5] = rect.vy;
        dst[6] = rect.lineWidth;
        dst[7] = rect.dashLength;

        dst[8] = rect.dashGap;
        dst[9] = rect.blinkPeriod;
        dst[10] = rect.blinkDuty;
        dst[11] = 0.0f;

        dst[12] = rect.color[0];
        dst[13] = rect.color[1];
        dst[14] = rect.color[2];
        dst[15] = rect.color[3];

        data += instanceStride;
    }

    float rectMaxParams[4] = { float(m_rectMaxTextureWidth), 0.0f, 0.0f, 0.0f };
    if (m_rectMaxTextureWidth > 0)
    {
        rectMaxParams[1] = 1.0f / float(m_rectMaxTextureWidth);
    }

    const float overlayZLift = std::max(0.003f, dmapScale() * 0.03f);
    const float rectViewParams[4] = { float(m_width), float(m_height), m_overlayTime, overlayZLift };
    const float rectDebugParams[4] = {
        m_overlayDebugAxes ? 1.0f : 0.0f,
        6.0f,
        10.0f,
        14.0f
    };

    bgfx::setUniform(m_rectMaxParamsHandle, rectMaxParams);
    bgfx::setUniform(m_rectViewParamsHandle, rectViewParams);
    bgfx::setUniform(m_rectDebugParamsHandle, rectDebugParams);
    bgfx::setTexture(2, m_rectMaxSampler, m_rectMaxTexture,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);

    float model[16];
    buildModelMatrix(model);
    bgfx::setTransform(model);

    bgfx::setVertexBuffer(0, m_rectWireVertices);
    bgfx::setIndexBuffer(m_rectWireIndices);
    bgfx::setInstanceDataBuffer(&idb);

    bgfx::setState(BGFX_STATE_WRITE_RGB
        | BGFX_STATE_WRITE_A
        | BGFX_STATE_DEPTH_TEST_LESS
        | BGFX_STATE_BLEND_ALPHA
        | BGFX_STATE_MSAA);

    bgfx::submit(m_viewId, m_programRectWire);
}
