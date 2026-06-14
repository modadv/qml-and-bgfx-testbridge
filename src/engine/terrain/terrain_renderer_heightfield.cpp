// terrain_renderer_heightfield.cpp
//
// Heightfield + diffuse asset pipeline for TerrainRenderer: file load, async
// worker decode (HeightfieldTextureLoader), GPU/CPU decode dispatch, dmap/diffuse
// texture upload, and the LRU texture cache. Split out of the TerrainRenderer god
// class; implements methods declared in terrain_renderer.h.

#include "terrain_renderer.h"
#include "terrain_patch_tables.h"
#include "terrain_types.h"
#include "logger.h"
#include "common/bgfx_utils.h"
#include "render_device.h"
#include "terrain_cpu_compute.h"
#include "heightfield_asset.h"
#include "render_capabilities.h"
#include <bimg/decode.h>
#include <bimg/bimg.h>
#include <bgfx/bgfx.h>
#include <bx/file.h>
#include <bx/math.h>
#include <bx/allocator.h>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>
#include <algorithm>
#include <fstream>
#include <deque>
#include <mutex>
#include <chrono>

namespace {
constexpr HeightfieldTextureLoader::DecodeMode kHeightfieldDecodeMode = HeightfieldTextureLoader::DecodeMode::Float32;
constexpr uint8_t kHeightfieldOrderIdBGRA = 0;
constexpr uint8_t kHeightfieldOrderIdRGBA = 1;


bool probeImageSize(const char* path, uint16_t& outW, uint16_t& outH)
{
    outW = 0;
    outH = 0;
    if (path == nullptr || *path == '\0')
        return false;

    bx::FileReader reader;
    if (!bx::open(&reader, path))
        return false;

    const uint32_t size = uint32_t(bx::getSize(&reader));
    if (size == 0)
    {
        bx::close(&reader);
        return false;
}

    std::vector<uint8_t> data(size);
    const int32_t readSize = bx::read(&reader, data.data(), int32_t(size), bx::ErrorAssert{});
    bx::close(&reader);
    if (readSize <= 0)
        return false;

    bx::DefaultAllocator allocator;
    bimg::ImageContainer* img = bimg::imageParse(&allocator, data.data(), uint32_t(readSize));
    if (!img)
        return false;

    outW = img->m_width;
    outH = img->m_height;
    bimg::imageFree(img);
    return outW > 0 && outH > 0;
}

bool needSwapUvByOrientation(uint16_t diffuseW,
                             uint16_t diffuseH,
                             uint16_t heightfieldW,
                             uint16_t heightfieldH)
{
    if (heightfieldW == 0 || heightfieldH == 0 || diffuseW == 0 || diffuseH == 0)
        return false;

    const bool hmLandscape = heightfieldW > heightfieldH;
    const bool hmPortrait = heightfieldH > heightfieldW;
    const bool dfLandscape = diffuseW > diffuseH;
    const bool dfPortrait = diffuseH > diffuseW;

    // If either image is square-like, keep current mapping and avoid unstable toggles.
    if ((!hmLandscape && !hmPortrait) || (!dfLandscape && !dfPortrait))
        return false;

    // Orientation mismatch means UV axes are opposite and should be swapped.
    return (hmLandscape && dfPortrait) || (hmPortrait && dfLandscape);
}

TerrainRenderer::DiffuseUvMode chooseDiffuseUvMode(uint16_t diffuseW,
                                                     uint16_t diffuseH,
                                                     uint16_t heightfieldW,
                                                     uint16_t heightfieldH)
{
    if (heightfieldW == 0 || heightfieldH == 0 || diffuseW == 0 || diffuseH == 0)
        return TerrainRenderer::DiffuseUvMode::None;

    if (needSwapUvByOrientation(diffuseW, diffuseH, heightfieldW, heightfieldH))
    {
        return TerrainRenderer::DiffuseUvMode::SwapUV;
    }
    return TerrainRenderer::DiffuseUvMode::None;
}

// Custom heightfield stores per-pixel 4-byte payloads; decode mode/order are hardcoded.
constexpr int32_t kHeightSentinelMin = -1000000;
constexpr int32_t kHeightSentinelMax = 1000000;
constexpr int32_t kHeightSentinelInvalid = -500000;

inline bool isSentinelHeight(int32_t value)
{
    return value == kHeightSentinelMin
        || value == kHeightSentinelMax
        || value == kHeightSentinelInvalid;
}

inline float decodeLegacyHeightValue(int32_t value)
{
    if (isSentinelHeight(value))
        return std::numeric_limits<float>::quiet_NaN();

    return float(value) / 100.0f;
}
}

void TerrainRenderer::loadDiffuseTexture() {
    const char* filePath = m_diffuseTexturePath;

    if (!filePath || filePath[0] == '\0') {
        const bgfx::Memory* mem = bgfx::alloc(4);
        uint8_t* data = mem->data;
        data[0] = data[1] = data[2] = 128;
        data[3] = 255;

        bgfx::TextureHandle newDiffuseTexture = bgfx::createTexture2D(
            1, 1, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_NONE, mem
        );

        deferDestroyTexture(m_textures[types::TEXTURE_DIFFUSE], 5);

        m_textures[types::TEXTURE_DIFFUSE] = newDiffuseTexture;
        m_diffuseUvMode = DiffuseUvMode::None;
        return;
    }

    uint64_t textureFlags = BGFX_TEXTURE_NONE
        | BGFX_SAMPLER_UVW_BORDER
        | BGFX_SAMPLER_MIN_ANISOTROPIC
        | BGFX_SAMPLER_MAG_ANISOTROPIC
        | BGFX_SAMPLER_MIP_SHIFT;

    bgfx::TextureHandle newDiffuseTexture = loadTexture(filePath, textureFlags);

    if (!bgfx::isValid(newDiffuseTexture)) {
        BX_TRACE("Failed to load diffuse texture: %s, using default texture", filePath);

        const bgfx::Memory* mem = bgfx::alloc(4);
        uint8_t* data = mem->data;
        data[0] = data[1] = data[2] = 128;
        data[3] = 255;

        newDiffuseTexture = bgfx::createTexture2D(
            1, 1, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_NONE, mem
        );
    }

    deferDestroyTexture(m_textures[types::TEXTURE_DIFFUSE], 5);

    m_textures[types::TEXTURE_DIFFUSE] = newDiffuseTexture;

    uint16_t diffuseW = 0;
    uint16_t diffuseH = 0;
    m_diffuseUvMode = DiffuseUvMode::None;
    if (probeImageSize(filePath, diffuseW, diffuseH)
        && m_heightfieldWidth > 0 && m_heightfieldHeight > 0)
    {
        m_diffuseUvMode = chooseDiffuseUvMode(
            diffuseW, diffuseH, m_heightfieldWidth, m_heightfieldHeight);
        LOG_I("[TerrainRenderer] Diffuse/heightfield size check diffuse={}x{}, heightfield={}x{}, uvMode={}",
              diffuseW, diffuseH, m_heightfieldWidth, m_heightfieldHeight, int(m_diffuseUvMode));
}
}

bool TerrainRenderer::loadHeightfieldFromFile(const char* localPath)
{
    if (localPath == nullptr || *localPath == '\0')
    {
        clearHeightfield();
        return true;
    }

    bx::strCopy(m_heightfieldPath, sizeof(m_heightfieldPath), localPath);

    m_dmapConfig.pathToFile = bx::FilePath(localPath);

    m_heightfieldNeedReload = true;
    m_heightfieldReady = false;
    m_rectComputeDirty = true;
    return true;
}


bool TerrainRenderer::loadDiffuseFromFile(const char* localPath)
{
    if (localPath == nullptr || *localPath == '\0') {
        clearDiffuse();
        return true;
    }
    bx::strCopy(m_diffuseTexturePath, BX_COUNTOF(m_diffuseTexturePath), localPath);

    m_diffuseNeedReload = true;
    return true;
}

void TerrainRenderer::clearHeightfield()
{
    m_heightfieldPath[0] = '\0';
    m_dmapConfig.pathToFile = bx::FilePath("");
    m_heightfieldNeedReload = true;
    m_heightfieldReady = false;
    m_heightfieldWidth = 0;
    m_heightfieldHeight = 0;
    m_heightfieldMips = 1;
    m_terrainAspectRatio = 1.0f;
    m_heightValueBias = 0.0f;
    m_heightValueScale = 0.3f;
    m_pendingHeightfieldGpuDecodes.clear();
    {
        std::lock_guard<std::mutex> lock(m_uploadMutex);
        m_asyncUploadQueue.clear();
    }
    m_heightfieldCpu.clear();
    m_heightfieldCpuWidth = 0;
    m_heightfieldCpuHeight = 0;
    m_overlayWorldDirty = true;
    m_rectComputeDirty = true;
    if (m_rectMaxReadPending)
    {
        m_rectMaxReadCancelPending = true;
    }
    m_rectMaxReadRequested = false;
    m_rectMaxHeights.clear();
    m_diffuseUvMode = DiffuseUvMode::None;

    deferDestroyTexture(m_textures[types::TEXTURE_DMAP], 5);
    m_textures[types::TEXTURE_DMAP] = BGFX_INVALID_HANDLE;
    deferDestroyTexture(m_textures[types::TEXTURE_SMAP], 5);
    m_textures[types::TEXTURE_SMAP] = BGFX_INVALID_HANDLE;
}

void TerrainRenderer::clearDiffuse()
{
    m_diffuseTexturePath[0] = '\0';
    m_diffuseNeedReload = true;
    m_diffuseUvMode = DiffuseUvMode::None;

    deferDestroyTexture(m_textures[types::TEXTURE_DIFFUSE], 5);
    m_textures[types::TEXTURE_DIFFUSE] = BGFX_INVALID_HANDLE;
}

HeightfieldTextureLoader::LoadRequest HeightfieldTextureLoader::loadImageData(const std::string& path, bool preferGpuDecode)
{
    const int64_t loadStart = bx::getHPCounter();
    LoadRequest request;
    request.path = path;
    request.success = false;

    engine::terrain::HeightfieldLoadResult loaded =
        engine::terrain::loadHeightfieldAsset(path, preferGpuDecode);

    if (!loaded.success)
    {
        LOG_W("[HeightfieldTextureLoader] Failed to load heightfield '{}': {}", path, loaded.error);
        request.width = 1;
        request.height = 1;
        request.data.resize(1, 0);
        request.aspectRatio = 1.0f;
        request.formatName = "fallback";
        request.success = true;
        return request;
    }

    const engine::terrain::HeightfieldAsset& asset = loaded.asset;
    request.width = int(asset.width);
    request.height = int(asset.height);
    request.heightMin = asset.heightMin;
    request.heightMax = asset.heightMax;
    request.data = asset.samples;
    request.rawData = asset.rawData;
    request.rawFormat = asset.rawFormat;
    request.rawIsBGRA = asset.rawIsBGRA;
    request.decodeMode = kHeightfieldDecodeMode;
    request.decodeOrder = request.rawIsBGRA ? kHeightfieldOrderIdBGRA : kHeightfieldOrderIdRGBA;
    request.aspectRatio = asset.aspectRatio;
    request.formatName = asset.formatName;
    request.sampleType = asset.sampleType;
    request.success = true;

    const int64_t loadEnd = bx::getHPCounter();
    const float loadMs = float((loadEnd - loadStart) / double(bx::getHPFrequency()) * 1000.0);
    LOG_I("[HeightfieldTextureLoader] Loaded heightfield: {} format={} ({}x{}, aspect ratio={:.3f}, heightRange=[{:.3f},{:.3f}], load_ms={:.3f})",
          path,
          request.formatName,
          request.width,
          request.height,
          request.aspectRatio,
          request.heightMin,
          request.heightMax,
          loadMs);
    return request;
}

bool HeightfieldTextureLoader::decodeCustomHeightfield(const uint8_t* imageData, int width, int height,
                                                   int channels, bimg::TextureFormat::Enum format,
                                                   std::vector<float>& heightMap)
{
    if (!imageData || width <= 0 || height <= 0)
        return false;

    if (channels == 3)
    {
        if (decodeBGRHeightfield(imageData, width, height, heightMap))
            return true;

        std::vector<uint8_t> swapped(size_t(width) * size_t(height) * 3);
        for (int i = 0; i < width * height; ++i)
        {
            swapped[i * 3 + 0] = imageData[i * 3 + 2];
            swapped[i * 3 + 1] = imageData[i * 3 + 1];
            swapped[i * 3 + 2] = imageData[i * 3 + 0];
        }
        return decodeBGRHeightfield(swapped.data(), width, height, heightMap);
    }

    if (channels == 4)
    {
        const bool isBGRA = (format == bimg::TextureFormat::BGRA8);
        return decodeRGBAHeightfield(imageData, width, height, isBGRA, heightMap);
    }

    return false;
}

bool HeightfieldTextureLoader::decodeBGRHeightfield(const uint8_t* imageData, int width, int height,
                                                std::vector<float>& heightMap)
{
    if (!imageData || width <= 0 || height <= 0)
        return false;

    const size_t pixelCount = size_t(width) * size_t(height);
    heightMap.assign(pixelCount, 0.0f);

    const size_t rowStride = size_t(width) * 3;
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* row = imageData + size_t(y) * rowStride;
        for (int x = 0; x < width; ++x)
        {
            const uint8_t* pixel = row + size_t(x) * 3;
            uint32_t b = pixel[0];
            uint32_t g = pixel[1];
            uint32_t r = pixel[2];
            int32_t valInt = int32_t((r << 16) | (g << 8) | b);

            if (valInt & (1 << 23))
            {
                valInt &= ~(1 << 23);
                valInt = -valInt;
            }

            heightMap[size_t(y) * size_t(width) + size_t(x)] = decodeLegacyHeightValue(valInt);
        }
    }

    return true;
}

bool HeightfieldTextureLoader::decodeRGBAHeightfield(const uint8_t* imageData, int width, int height,
                                                 bool isBGRA,
                                                 std::vector<float>& heightMap)
{
    if (!imageData || width <= 0 || height <= 0)
        return false;

    const size_t pixelCount = size_t(width) * size_t(height);
    struct ByteOrder {
        int b0;
        int b1;
        int b2;
        int b3;
    };

    auto decodeFloat32 = [&](const ByteOrder& order, std::vector<float>& out) {
        out.resize(pixelCount);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            uint32_t packed =
                (uint32_t(imageData[i * 4 + order.b0]) << 0) |
                (uint32_t(imageData[i * 4 + order.b1]) << 8) |
                (uint32_t(imageData[i * 4 + order.b2]) << 16) |
                (uint32_t(imageData[i * 4 + order.b3]) << 24);
            float value = 0.0f;
            std::memcpy(&value, &packed, sizeof(float));
            if (value == float(kHeightSentinelMin)
                || value == float(kHeightSentinelMax)
                || value == float(kHeightSentinelInvalid))
            {
                value = std::numeric_limits<float>::quiet_NaN();
            }
            out[i] = value;
        }
    };

    auto decodeInt32 = [&](const ByteOrder& order, std::vector<float>& out) {
        out.resize(pixelCount);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            uint32_t packed =
                (uint32_t(imageData[i * 4 + order.b0]) << 0) |
                (uint32_t(imageData[i * 4 + order.b1]) << 8) |
                (uint32_t(imageData[i * 4 + order.b2]) << 16) |
                (uint32_t(imageData[i * 4 + order.b3]) << 24);
            int32_t value = static_cast<int32_t>(packed);
            out[i] = decodeLegacyHeightValue(value);
        }
    };

    auto orderFromId = [](uint8_t id) -> ByteOrder {
        switch (id & 3)
        {
        case 1: return {2, 1, 0, 3};
        case 2: return {3, 2, 1, 0};
        case 3: return {3, 0, 1, 2};
        default: return {0, 1, 2, 3};
        }
    };
    const uint8_t orderId = isBGRA ? kHeightfieldOrderIdBGRA : kHeightfieldOrderIdRGBA;
    const ByteOrder order = orderFromId(orderId);
    if (kHeightfieldDecodeMode == HeightfieldTextureLoader::DecodeMode::Float32)
        decodeFloat32(order, heightMap);
    else
        decodeInt32(order, heightMap);
    const char* modeName = kHeightfieldDecodeMode == HeightfieldTextureLoader::DecodeMode::Float32 ? "float32" : "int32";
    const char* formatName = isBGRA ? "BGRA8" : "RGBA8";
    LOG_I("[HeightfieldTextureLoader] CPU decode forced: mode={}, orderId={}, format={}",
          modeName, int(orderId), formatName);
    return true;
}

void HeightfieldTextureLoader::convertToUint16Heightfield(const std::vector<float>& heightMap,
                                                      std::vector<uint16_t>& output,
                                                      float& minHeight, float& maxHeight)
{
    if (heightMap.empty() || output.size() != heightMap.size())
        return;

    // Robust min/max: ignore outlier clusters (e.g. 430-platform pixels saturated
    // near int16 minimum) that are not in the canonical sentinel list. Falls back
    // to plain min/max when no outliers are detected.
    minHeight = 0.0f;
    maxHeight = 0.0f;
    const bool haveRange = engine::computeRobustHeightRange(
        heightMap.data(), heightMap.size(), minHeight, maxHeight);

    if (!haveRange || maxHeight <= minHeight)
    {
        std::fill(output.begin(), output.end(), 0);
        minHeight = 0.0f;
        maxHeight = 0.0f;
        return;
    }

    // Always shift so the lowest in-range height maps to dmap=0. This is what
    // anchors the model bottom to z=0 in the vertex shader (z = dmap*factor+bias
    // with bias=0). Earlier we clamped negative heightMin to 0, but datasets
    // commonly have heightMin > 0 (e.g. 92x52 component crops with min=512µm,
    // max=7576µm) — without shift those models would float ~512µm above z=0.
    const bool shift = true;
    const float range = maxHeight - minHeight;
    if (range <= 0.0f)
    {
        std::fill(output.begin(), output.end(), 0);
        return;
    }

    // Invalid (NaN/Inf) pixels normalize to 0 so they sit on the base plane,
    // matching the GPU normalize shader. (Previously this used (-minHeight)/range,
    // which only made sense when shift was conditional on minHeight<0.)
    float invalidNormalized = 0.0f;

    for (size_t i = 0; i < heightMap.size(); ++i)
    {
        float v = heightMap[i];
        if (!std::isfinite(v))
        {
            output[i] = static_cast<uint16_t>(invalidNormalized);
            continue;
        }
        // Out-of-robust-range outliers (e.g. 430-platform pixels saturated near
        // int16 minimum) get clamped to 0 or 65535 by the normalize math below.
        // This matches what the GPU normalize shader does (clamp 0..1) so CPU
        // and GPU decode paths visualize the same way.
        float shifted = shift ? (v - minHeight) : v;
        float normalized = shifted / range * 65535.0f;
        if (normalized < 0.0f)
            normalized = 0.0f;
        else if (normalized > 65535.0f)
            normalized = 65535.0f;
        output[i] = static_cast<uint16_t>(normalized);
    }
}


void TerrainRenderer::loadDmapTexture()
{
    const char* path = m_dmapConfig.pathToFile.getCPtr();
    std::string pathStr = path ? path : "";

    m_textureLoader->loadTexture(pathStr, canUseGpuHeightfieldDecode());
}

void TerrainRenderer::uploadLoadedTexture(HeightfieldTextureLoader::LoadRequest&& request)
{
    if (!request.success) {
        return;
    }

    // First try to restore a CPU-backed cache entry.
    if (tryLoadFromCache(request.path)) {
        LOG_D("[TerrainRenderer] Loaded heightfield from cache: {}", request.path);
        return;
    }

    // Prefer the GPU decode path when the active backend supports it.
    const bool canGpuDecode = canUseGpuHeightfieldDecode();
    if (!request.rawData.empty() && canGpuDecode)
    {
        m_heightfieldCpu.clear();
        m_heightfieldCpuWidth = 0;
        m_heightfieldCpuHeight = 0;
        m_pendingHeightfieldGpuDecodes.clear();
        m_pendingHeightfieldGpuDecodes.push_back(std::move(request));
        return;
    }

    if (!request.rawData.empty() && !canGpuDecode)
    {
        LOG_W("[TerrainRenderer] GPU decode disabled, reloading heightfield on CPU: {}", request.path);
        m_textureLoader->loadTexture(request.path, false);
        return;
    }

    if (request.data.empty())
    {
        LOG_W("[TerrainRenderer] Missing CPU heightfield data for '{}', skipping upload", request.path);
        return;
    }

    // Queue a CPU-decoded upload for the render thread.
    AsyncUploadTask task;
    task.path = request.path;
    task.data = std::move(request.data);
    task.width = request.width;
    task.height = request.height;
    task.heightMin = request.heightMin;
    task.heightMax = request.heightMax;
    task.aspectRatio = request.aspectRatio;

    {
        std::lock_guard<std::mutex> lock(m_uploadMutex);
        m_asyncUploadQueue.push_back(std::move(task));
    }

    LOG_D("[TerrainRenderer] Queued async upload for: {}", request.path);
}

void TerrainRenderer::processPendingGpuDecodes()
{
    if (m_pendingHeightfieldGpuDecodes.empty())
        return;

    HeightfieldTextureLoader::LoadRequest request = std::move(m_pendingHeightfieldGpuDecodes.back());
    m_pendingHeightfieldGpuDecodes.clear();

    if (!dispatchGpuHeightfieldDecode(std::move(request)))
    {
        LOG_W("[TerrainRenderer] GPU heightfield decode failed, keeping previous DMap");
    }
}

bool TerrainRenderer::canUseGpuHeightfieldDecode() const
{
    if (RenderDevice::renderCaps().noCompute())
        return false;

    if (!m_useGpuHeightfieldDecode)
        return false;

    const bgfx::Caps* caps = bgfx::getCaps();
    if (!caps || (caps->supported & BGFX_CAPS_COMPUTE) == 0)
        return false;
    if (caps->rendererType == bgfx::RendererType::OpenGL
        || caps->rendererType == bgfx::RendererType::OpenGLES)
    {
        static bool logged = false;
        if (!logged)
        {
            LOG_W("[TerrainRenderer] GPU heightfield decode disabled on OpenGL backend");
            logged = true;
        }
        return false;
    }

    if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA32F, BGFX_TEXTURE_COMPUTE_WRITE))
        return false;
    if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::R32F, BGFX_TEXTURE_COMPUTE_WRITE))
        return false;

    return true;
}

bool TerrainRenderer::dispatchGpuHeightfieldDecode(HeightfieldTextureLoader::LoadRequest&& request)
{
    if (request.rawData.empty() || request.width <= 0 || request.height <= 0)
        return false;

    if (!canUseGpuHeightfieldDecode())
        return false;

    if (!bgfx::isValid(m_programsCompute[types::PROGRAM_HEIGHTFIELD_MINMAX]) ||
        !bgfx::isValid(m_programsCompute[types::PROGRAM_HEIGHTFIELD_REDUCE]) ||
        !bgfx::isValid(m_programsCompute[types::PROGRAM_HEIGHTFIELD_NORMALIZE]) ||
        !bgfx::isValid(m_heightfieldDecodeParamsHandle) ||
        !bgfx::isValid(m_samplers[types::HEIGHTFIELD_RAW_SAMPLER]))
    {
        return false;
    }

    const uint16_t w = uint16_t(request.width);
    const uint16_t h = uint16_t(request.height);
    constexpr uint16_t kGroupSize = 16;
    const uint16_t groupsX = uint16_t((w + kGroupSize - 1) / kGroupSize);
    const uint16_t groupsY = uint16_t((h + kGroupSize - 1) / kGroupSize);

    bgfx::TextureFormat::Enum rawFormat = bgfx::TextureFormat::BGRA8;
    if (request.rawFormat == bimg::TextureFormat::RGBA8)
        rawFormat = bgfx::TextureFormat::RGBA8;

    const char* modeName = request.decodeMode == HeightfieldTextureLoader::DecodeMode::Float32 ? "float32" : "int32";
    const char* formatName = request.rawFormat == bimg::TextureFormat::RGBA8 ? "RGBA8" : "BGRA8";
    LOG_I("[TerrainRenderer] GPU heightfield decode: mode={}, orderId={}, format={}",
          modeName, int(request.decodeOrder), formatName);

    const bgfx::Memory* rawMem = bgfx::copy(request.rawData.data(), uint32_t(request.rawData.size()));
    bgfx::TextureHandle rawTexture = bgfx::createTexture2D(
        w, h, false, 1, rawFormat,
        BGFX_TEXTURE_NONE, rawMem
    );
    if (!bgfx::isValid(rawTexture))
    {
        LOG_W("[TerrainRenderer] Failed to create raw heightfield texture for '{}'", request.path);
        return false;
    }

    const uint16_t minmaxW = uint16_t((w + kGroupSize - 1) / kGroupSize);
    const uint16_t minmaxH = uint16_t((h + kGroupSize - 1) / kGroupSize);

    const uint64_t rawSamplerFlags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
        | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;

    const float decodeParams[4] = {
        float(w),
        float(h),
        request.decodeMode == HeightfieldTextureLoader::DecodeMode::Float32 ? 1.0f : 0.0f,
        float(request.decodeOrder + (request.rawIsBGRA ? 0 : 4))
    };

    // The CPU side (engine::computeRobustHeightRange via convertToUint16Heightfield)
    // already computed an outlier-trimmed [heightMin, heightMax]. When that
    // range is available we skip the GPU reduction pipeline entirely and
    // upload the robust values directly into a 1x1 minmax texture. This both
    // avoids redundant GPU work and prevents outlier clusters (e.g. 430-platform
    // pixels saturated near int16 minimum) from crushing the displayed 3D
    // height into a flat band.
    const bool useRobustMinmax = std::isfinite(request.heightMin)
        && std::isfinite(request.heightMax)
        && request.heightMax > request.heightMin;

    bgfx::TextureHandle minmaxIn = BGFX_INVALID_HANDLE;
    uint16_t inW = 0;
    uint16_t inH = 0;

    if (useRobustMinmax)
    {
        minmaxIn = bgfx::createTexture2D(
            1, 1, false, 1, bgfx::TextureFormat::RGBA32F,
            BGFX_TEXTURE_NONE
        );
        if (!bgfx::isValid(minmaxIn))
        {
            bgfx::destroy(rawTexture);
            LOG_W("[TerrainRenderer] Failed to create robust min/max texture for '{}', falling back to GPU reduction",
                  request.path);
        }
        else
        {
            const float minmaxData[4] = {
                request.heightMin,
                request.heightMax,
                0.0f,
                0.0f
            };
            const bgfx::Memory* mem = bgfx::copy(minmaxData, sizeof(minmaxData));
            bgfx::updateTexture2D(minmaxIn, 0, 0, 0, 0, 1, 1, mem);
            inW = 1;
            inH = 1;
            LOG_I("[TerrainRenderer] Using robust CPU minmax (skipping GPU reduction): [{:.4f}, {:.4f}]",
                  request.heightMin, request.heightMax);
        }
    }

    if (!bgfx::isValid(minmaxIn))
    {
        const uint64_t minmaxFlags = BGFX_TEXTURE_COMPUTE_WRITE;
        bgfx::TextureHandle minmaxTexture = bgfx::createTexture2D(
            minmaxW, minmaxH, false, 1, bgfx::TextureFormat::RGBA32F,
            minmaxFlags
        );
        if (!bgfx::isValid(minmaxTexture))
        {
            bgfx::destroy(rawTexture);
            LOG_W("[TerrainRenderer] Failed to create min/max texture for '{}'", request.path);
            return false;
        }

        bgfx::setUniform(m_heightfieldDecodeParamsHandle, decodeParams);
        bgfx::setTexture(0, m_samplers[types::HEIGHTFIELD_RAW_SAMPLER], rawTexture, rawSamplerFlags);
        bgfx::setImage(1, minmaxTexture, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA32F);
        bgfx::dispatch(m_viewId, m_programsCompute[types::PROGRAM_HEIGHTFIELD_MINMAX], minmaxW, minmaxH, 1);

        minmaxIn = minmaxTexture;
        inW = minmaxW;
        inH = minmaxH;
        while (inW > 1 || inH > 1)
        {
            uint16_t outW = uint16_t((inW + kGroupSize - 1) / kGroupSize);
            uint16_t outH = uint16_t((inH + kGroupSize - 1) / kGroupSize);
            bgfx::TextureHandle minmaxOut = bgfx::createTexture2D(
                outW, outH, false, 1, bgfx::TextureFormat::RGBA32F, minmaxFlags
            );
            if (!bgfx::isValid(minmaxOut))
            {
                bgfx::destroy(minmaxIn);
                bgfx::destroy(rawTexture);
                LOG_W("[TerrainRenderer] Failed to create reduction texture for '{}'", request.path);
                return false;
            }

            bgfx::setImage(0, minmaxIn, 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA32F);
            bgfx::setImage(1, minmaxOut, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA32F);
            bgfx::dispatch(m_viewId, m_programsCompute[types::PROGRAM_HEIGHTFIELD_REDUCE], outW, outH, 1);

            bgfx::destroy(minmaxIn);
            minmaxIn = minmaxOut;
            inW = outW;
            inH = outH;
        }
    }

    bgfx::TextureHandle newDmapTexture = bgfx::createTexture2D(
        w, h, false, 1, bgfx::TextureFormat::R32F,
        BGFX_TEXTURE_COMPUTE_WRITE
    );
    if (!bgfx::isValid(newDmapTexture))
    {
        bgfx::destroy(minmaxIn);
        bgfx::destroy(rawTexture);
        LOG_W("[TerrainRenderer] Failed to create GPU DMap texture for '{}'", request.path);
        return false;
    }

    bgfx::setUniform(m_heightfieldDecodeParamsHandle, decodeParams);
    bgfx::setTexture(0, m_samplers[types::HEIGHTFIELD_RAW_SAMPLER], rawTexture, rawSamplerFlags);
    bgfx::setImage(1, minmaxIn, 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA32F);
    bgfx::setImage(2, newDmapTexture, 0, bgfx::Access::Write, bgfx::TextureFormat::R32F);
    bgfx::dispatch(m_viewId, m_programsCompute[types::PROGRAM_HEIGHTFIELD_NORMALIZE], groupsX, groupsY, 1);

    bgfx::destroy(minmaxIn);
    bgfx::destroy(rawTexture);

    deferDestroyTexture(m_textures[types::TEXTURE_DMAP], 60);

    const uint16_t prevWidth = m_heightfieldWidth;
    const uint16_t prevHeight = m_heightfieldHeight;
    const float prevAspect = m_terrainAspectRatio;

    m_textures[types::TEXTURE_DMAP] = newDmapTexture;
    m_heightfieldWidth = w;
    m_heightfieldHeight = h;
    m_heightfieldMips = 1;
    m_terrainAspectRatio = request.aspectRatio;
    m_heightfieldReady = (m_heightfieldPath[0] != '\0');
    // Anchor model bottom to z=0. convertToUint16Heightfield always shifts by
    // heightMin so dmap=0 maps to the lowest in-range height. The GPU normalize
    // shader does the same. Vertex shader z = dmap*factor + bias, so bias=0
    // and factor = (heightMax - heightMin) puts z_min on the base plane.
    m_heightValueBias = 0.0f;
    m_heightValueScale = request.heightMax - request.heightMin;
    if (m_heightValueScale <= 0.0f)
    {
        m_heightValueScale = 0.3f;
        m_heightValueBias = 0.0f;
    }
    m_smapNeedsRegen = true;
    m_rectComputeDirty = true;
    m_heightfieldCpu.clear();
    m_heightfieldCpuWidth = 0;
    m_heightfieldCpuHeight = 0;
    if (prevWidth != m_heightfieldWidth
        || prevHeight != m_heightfieldHeight
        || std::fabs(prevAspect - m_terrainAspectRatio) > 0.0001f)
    {
        m_overlayWorldDirty = true;
    }

    BX_TRACE("Uploaded GPU heightfield texture: %s (%dx%d), aspect ratio: %.2f",
             request.path.c_str(), request.width, request.height, m_terrainAspectRatio);

    if (m_diffuseTexturePath[0] != '\0')
    {
        uint16_t diffuseW = 0;
        uint16_t diffuseH = 0;
        if (probeImageSize(m_diffuseTexturePath, diffuseW, diffuseH))
        {
            m_diffuseUvMode = chooseDiffuseUvMode(
                diffuseW, diffuseH, m_heightfieldWidth, m_heightfieldHeight);
            LOG_I("[TerrainRenderer] Recompute uvMode after GPU heightfield upload: diffuse={}x{}, heightfield={}x{}, uvMode={}",
                  diffuseW, diffuseH, m_heightfieldWidth, m_heightfieldHeight, int(m_diffuseUvMode));
        }
    }
    return true;
}

// Asset cache and asynchronous upload implementation.

void TerrainRenderer::processAsyncUploads()
{
    AsyncUploadTask task;
    {
        std::lock_guard<std::mutex> lock(m_uploadMutex);
        if (m_asyncUploadQueue.empty()) {
            return;
        }
        task = std::move(m_asyncUploadQueue.front());
        m_asyncUploadQueue.pop_front();
    }
    const int64_t uploadStart = bx::getHPCounter();

    // Create bgfx textures on the render thread after worker-side decoding.
    m_heightfieldCpu = task.data;
    m_heightfieldCpuWidth = uint16_t(task.width);
    m_heightfieldCpuHeight = uint16_t(task.height);

    const bool noComputeUpload = RenderDevice::renderCaps().noCompute();
    bgfx::TextureHandle newDmapTexture = BGFX_INVALID_HANDLE;

    if (noComputeUpload)
    {
        // NoCompute path still needs a CPU-side normalized copy for smap
        // regeneration and rect_max computation.
        const size_t pixelCount = size_t(task.width) * size_t(task.height);
        m_dmapNormalizedCpu.resize(pixelCount);
        const float invMax = 1.0f / 65535.0f;
        const size_t safeCount = std::min(pixelCount, task.data.size());
        for (size_t i = 0; i < safeCount; ++i)
        {
            m_dmapNormalizedCpu[i] = float(task.data[i]) * invMax;
        }
        for (size_t i = safeCount; i < pixelCount; ++i)
        {
            m_dmapNormalizedCpu[i] = 0.0f;
        }
    }

    // Mesa/Intel legacy GL path can reject R16 initial-data uploads at
    // odd widths (GL_UNPACK_ALIGNMENT mismatch: 193*2=386 bytes per row is
    // not 4-byte aligned). Create empty first, then update — bgfx sets
    // pixel-store alignment correctly inside updateTexture2D.
    LOG_I("[TerrainRenderer] >> createTexture2D DMAP R16 {}x{} flags=NONE (empty)",
          task.width, task.height);
    newDmapTexture = bgfx::createTexture2D(
        uint16_t(task.width), uint16_t(task.height),
        false, 1, bgfx::TextureFormat::R16,
        BGFX_TEXTURE_NONE
    );
    LOG_I("[TerrainRenderer] << createTexture2D DMAP R16 handle={} valid={}",
          newDmapTexture.idx, bgfx::isValid(newDmapTexture));
    if (bgfx::isValid(newDmapTexture))
    {
        const bgfx::Memory* mem = bgfx::copy(
            task.data.data(),
            uint32_t(task.data.size() * sizeof(uint16_t))
        );
        bgfx::updateTexture2D(newDmapTexture, 0, 0, 0, 0,
                              uint16_t(task.width), uint16_t(task.height),
                              mem);
        LOG_I("[TerrainRenderer]    updateTexture2D DMAP R16 {}x{} mem={}B",
              task.width, task.height, uint32_t(task.data.size() * sizeof(uint16_t)));
    }

    deferDestroyTexture(m_textures[types::TEXTURE_DMAP], 60);

    m_textures[types::TEXTURE_DMAP] = newDmapTexture;

    const uint16_t prevWidth = m_heightfieldWidth;
    const uint16_t prevHeight = m_heightfieldHeight;
    const float prevAspect = m_terrainAspectRatio;

    m_terrainAspectRatio = task.aspectRatio;
    m_heightfieldWidth = uint16_t(task.width);
    m_heightfieldHeight = uint16_t(task.height);
    m_heightfieldMips = 1;
    m_heightfieldReady = (m_heightfieldPath[0] != '\0');
    // Anchor model bottom to z=0; see request-path comment for the derivation.
    m_heightValueBias = 0.0f;
    m_heightValueScale = task.heightMax - task.heightMin;

    if (m_heightValueScale <= 0.0f)
    {
        m_heightValueScale = 0.3f;
        m_heightValueBias = 0.0f;
    }

    m_smapNeedsRegen = true;
    m_rectComputeDirty = true;

    if (m_rectMaxReadPending)
    {
        m_rectMaxReadCancelPending = true;
    }
    m_rectMaxReadRequested = true;

    if (prevWidth != m_heightfieldWidth
        || prevHeight != m_heightfieldHeight
        || std::fabs(prevAspect - m_terrainAspectRatio) > 0.0001f)
    {
        m_overlayWorldDirty = true;
    }

    if (m_diffuseTexturePath[0] != '\0')
    {
        uint16_t diffuseW = 0;
        uint16_t diffuseH = 0;
        if (probeImageSize(m_diffuseTexturePath, diffuseW, diffuseH))
        {
            m_diffuseUvMode = chooseDiffuseUvMode(
                diffuseW, diffuseH, m_heightfieldWidth, m_heightfieldHeight);
            LOG_I("[TerrainRenderer] Recompute uvMode after async heightfield upload: diffuse={}x{}, heightfield={}x{}, uvMode={}",
                  diffuseW, diffuseH, m_heightfieldWidth, m_heightfieldHeight, int(m_diffuseUvMode));
        }
    }

    CachedTexture cached;
    cached.width = uint16_t(task.width);
    cached.height = uint16_t(task.height);
    cached.aspectRatio = task.aspectRatio;
    cached.heightMin = task.heightMin;
    cached.heightMax = task.heightMax;
    cached.cpuData = std::move(task.data);
    cached.lastAccessTime = std::chrono::steady_clock::now().time_since_epoch().count();

    addToCache(task.path, cached);

    const int64_t uploadEnd = bx::getHPCounter();
    const float uploadMs = float((uploadEnd - uploadStart) / double(bx::getHPFrequency()) * 1000.0);
    LOG_I("[engine.perf] heightfield_upload_ms={:.3f} path={} size={}x{}",
          uploadMs,
          task.path,
          m_heightfieldWidth,
          m_heightfieldHeight);
    LOG_D("[TerrainRenderer] Async upload completed and cached: {}", task.path);
}

bool TerrainRenderer::tryLoadFromCache(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    auto it = m_textureCache.find(path);
    if (it == m_textureCache.end()) {
        return false;
    }

    CachedTexture& cached = it->second;
    cached.lastAccessTime = std::chrono::steady_clock::now().time_since_epoch().count();

    deferDestroyTexture(m_textures[types::TEXTURE_DMAP], 60);

    // Recreate GPU textures from CPU data; bgfx handles are never cached.
    auto* mem = bgfx::copy(cached.cpuData.data(), uint32_t(cached.cpuData.size() * sizeof(uint16_t)));
    bgfx::TextureHandle newHandle = bgfx::createTexture2D(
        cached.width, cached.height, false, 1, bgfx::TextureFormat::R16,
        BGFX_TEXTURE_NONE, mem);
    if (!bgfx::isValid(newHandle)) {
        LOG_E("[TerrainRenderer] Failed to recreate DMAP texture from cache: {}", path);
        return false;
    }
    m_textures[types::TEXTURE_DMAP] = newHandle;
    m_heightfieldCpu = cached.cpuData;
    m_heightfieldCpuWidth = cached.width;
    m_heightfieldCpuHeight = cached.height;

    // NoCompute path keeps a normalized [0,1] copy for cpuRegenerateSmap;
    // without it the smap stays empty and the terrain renders flat.
    if (RenderDevice::renderCaps().noCompute())
    {
        const size_t pixelCount = size_t(cached.width) * size_t(cached.height);
        m_dmapNormalizedCpu.resize(pixelCount);
        const float invMax = 1.0f / 65535.0f;
        const size_t safeCount = std::min(pixelCount, cached.cpuData.size());
        for (size_t i = 0; i < safeCount; ++i)
        {
            m_dmapNormalizedCpu[i] = float(cached.cpuData[i]) * invMax;
        }
        for (size_t i = safeCount; i < pixelCount; ++i)
        {
            m_dmapNormalizedCpu[i] = 0.0f;
        }
    }
    m_terrainAspectRatio = cached.aspectRatio;
    m_heightfieldWidth = cached.width;
    m_heightfieldHeight = cached.height;
    // Anchor model bottom to z=0; see request-path comment for the derivation.
    m_heightValueBias = 0.0f;
    m_heightValueScale = cached.heightMax - cached.heightMin;

    if (m_heightValueScale <= 0.0f)
    {
        m_heightValueScale = 0.3f;
        m_heightValueBias = 0.0f;
    }

    m_heightfieldReady = true;
    m_smapNeedsRegen = true;
    m_rectComputeDirty = true;

    return true;
}

void TerrainRenderer::addToCache(const std::string& path, const CachedTexture& texture)
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    if (m_textureCache.size() >= m_maxCacheSize) {
        pruneCache();
    }

    m_textureCache[path] = texture;
}

void TerrainRenderer::pruneCache()
{
    std::string oldestKey;
    uint64_t oldestTime = UINT64_MAX;

    for (const auto& pair : m_textureCache) {
        if (pair.second.lastAccessTime < oldestTime) {
            oldestTime = pair.second.lastAccessTime;
            oldestKey = pair.first;
        }
    }

    if (!oldestKey.empty()) {
        m_textureCache.erase(oldestKey);
        LOG_D("[TerrainRenderer] Pruned cache entry: {}", oldestKey);
    }
}
