#include "terrain_renderer.h"
#include "terrain_patch_tables.h"
#include "terrain_types.h"
#include "terrain_renderer_internal.h"
#include "logger.h"
#include "common/bgfx_utils.h"
#include "render_device.h"
#include "terrain_cpu_compute.h"
#include "heightfield_asset.h"
#include "performance_monitor.h"
#include "render_capabilities.h"
#include <bimg/decode.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <bx/timer.h>
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

bgfx::ShaderHandle loadShaderBinaryFile(const std::string& path,
                                         std::string& error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "failed to open compiled shader: " + path;
        return BGFX_INVALID_HANDLE;
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        error = "compiled shader is empty: " + path;
        return BGFX_INVALID_HANDLE;
    }
    file.seekg(0, std::ios::beg);

    std::vector<char> bytes(static_cast<size_t>(size));
    if (!file.read(bytes.data(), size)) {
        error = "failed to read compiled shader: " + path;
        return BGFX_INVALID_HANDLE;
    }

    const bgfx::Memory* mem = bgfx::copy(bytes.data(), uint32_t(bytes.size()));
    bgfx::ShaderHandle shader = bgfx::createShader(mem);
    if (!bgfx::isValid(shader)) {
        error = "bgfx::createShader failed for: " + path;
    }
    return shader;
}

}

TerrainRenderer::TerrainRenderer()
    : m_dmap(nullptr)
    , m_width(0)
    , m_height(0)
    , m_instancedMeshVertexCount(0)
    , m_instancedMeshPrimitiveCount(0)
    , m_shading(types::PROGRAM_TERRAIN)
    , m_pingPong(0)
    , m_terrainAspectRatio(1.0f)
    , m_primitivePixelLengthTarget(1.0f)
    , m_fovy(60.0f)
    , m_restart(true)
    , m_wireframe(false)
    , m_cull(true)
    , m_freeze(false)
    , m_useGpuSmap(true)
    , m_heightfieldNeedReload(false)
    , m_diffuseNeedReload(false)
    , m_loadStartTime(0)
    , m_firstFrameRendered(false)
    , m_loadTime(0.0f)
    , m_cpuSmapGenTime(0.0f)
    , m_gpuSmapGenTime(0.0f)
    , m_loadHistoryCount(0)
{
    for (uint32_t i = 0; i < types::TEXTURE_COUNT; ++i) {
        m_textures[i] = BGFX_INVALID_HANDLE;
    }
    // Initialize invalid handles
    for (uint32_t i = 0; i < types::PROGRAM_COUNT; ++i) {
        m_programsCompute[i] = BGFX_INVALID_HANDLE;
    }
    for (uint32_t i = 0; i < types::SHADING_COUNT; ++i) {
        m_programsDraw[i] = BGFX_INVALID_HANDLE;
    }
    for (uint32_t i = 0; i < types::TEXTURE_COUNT; ++i) {
        m_textures[i] = BGFX_INVALID_HANDLE;
    }
    for (uint32_t i = 0; i < types::SAMPLER_COUNT; ++i) {
        m_samplers[i] = BGFX_INVALID_HANDLE;
    }

    m_bufferSubd[0] = BGFX_INVALID_HANDLE;
    m_bufferSubd[1] = BGFX_INVALID_HANDLE;
    m_bufferCulledSubd = BGFX_INVALID_HANDLE;
    m_bufferCounter = BGFX_INVALID_HANDLE;
    m_geometryIndices = BGFX_INVALID_HANDLE;
    m_geometryVertices = BGFX_INVALID_HANDLE;
    m_instancedGeometryIndices = BGFX_INVALID_HANDLE;
    m_instancedGeometryVertices = BGFX_INVALID_HANDLE;
    m_dispatchIndirect = BGFX_INVALID_HANDLE;
    m_smapParamsHandle = BGFX_INVALID_HANDLE;
    m_smapChunkParamsHandle = BGFX_INVALID_HANDLE;
    m_diffuseUvParamsHandle = BGFX_INVALID_HANDLE;
    m_heightfieldDecodeParamsHandle = BGFX_INVALID_HANDLE;
    m_programRectWire = BGFX_INVALID_HANDLE;
    m_programColor = BGFX_INVALID_HANDLE;
    m_colorLayoutReady = false;
    m_rectWireVertices = BGFX_INVALID_HANDLE;
    m_rectWireIndices = BGFX_INVALID_HANDLE;
    m_rectParamsBuffer = BGFX_INVALID_HANDLE;
    m_rectMaxTexture = BGFX_INVALID_HANDLE;
    m_rectMaxReadTexture = BGFX_INVALID_HANDLE;
    m_rectMaxSampler = BGFX_INVALID_HANDLE;
    m_rectMaxParamsHandle = BGFX_INVALID_HANDLE;
    m_rectViewParamsHandle = BGFX_INVALID_HANDLE;
    m_rectParamsHandle = BGFX_INVALID_HANDLE;
    m_rectSampleParamsHandle = BGFX_INVALID_HANDLE;
    m_rectDebugParamsHandle = BGFX_INVALID_HANDLE;

    // Initialize paths
    m_heightfieldPath[0] = '\0';
    m_diffuseTexturePath[0] = '\0';
    
    m_textureLoader = std::unique_ptr<HeightfieldTextureLoader>(new HeightfieldTextureLoader());
}

TerrainRenderer::~TerrainRenderer() {
    shutdown();
}

bool TerrainRenderer::init(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
    m_loadStartTime = bx::getHPCounter();
    m_firstFrameRendered = false;

    // Track the render-device generation that owns these resources.
    m_bgfxGeneration = RenderDevice::instance().generation();

    try {
        loadPrograms();
        loadBuffers();
        createAtomicCounters();
        
        if (!bgfx::isValid(m_dummySmap)) {
            const bgfx::Memory* mem = bgfx::alloc(4 * sizeof(float));
            float* data = reinterpret_cast<float*>(mem->data);
            data[0] = 0.0f;
            data[1] = 0.0f;
            data[2] = 0.0f;
            data[3] = 0.0f;
            m_dummySmap = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA32F,
                                                BGFX_TEXTURE_NONE, mem);
        }
        if (RenderDevice::renderCaps().noCompute())
        {
            m_dispatchIndirect = BGFX_INVALID_HANDLE;
        }
        else
        {
            m_dispatchIndirect = bgfx::createIndirectBuffer(2);
        }

        m_resourcesValid = true;
        return true;
    }
    catch (...) {
        destroyAllResources();
        return false;
    }
}


bool TerrainRenderer::ensureValidResources()
{
    uint64_t currentGen = RenderDevice::instance().generation();

    if (currentGen != m_bgfxGeneration || !m_resourcesValid)
    {
        invalidateAllHandles();

        if (!init(m_width, m_height))
        {
            return false;
        }

        if (m_heightfieldPath[0] != '\0')
        {
            m_heightfieldNeedReload = true;
        }
        if (m_diffuseTexturePath[0] != '\0')
        {
            m_diffuseNeedReload = true;
        }
        
        m_bgfxGeneration = currentGen;
    }
    
    return true;
}

void TerrainRenderer::destroyAllResources()
{
    bool shouldDestroy = RenderDevice::instance().isInitialized() && 
                         RenderDevice::instance().generation() == m_bgfxGeneration &&
                         m_resourcesValid;

    if (shouldDestroy)
    {
        m_uniforms.destroy();

        for (uint32_t i = 0; i < types::TEXTURE_COUNT; ++i) {
            if (bgfx::isValid(m_textures[i])) {
                bgfx::destroy(m_textures[i]);
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_textureCache.clear();
        }

        for (uint32_t i = 0; i < types::SAMPLER_COUNT; ++i) {
            if (bgfx::isValid(m_samplers[i])) {
                bgfx::destroy(m_samplers[i]);
            }
        }

        for (uint32_t i = 0; i < types::PROGRAM_COUNT; ++i) {
            if (bgfx::isValid(m_programsCompute[i])) {
                bgfx::destroy(m_programsCompute[i]);
            }
        }
        for (uint32_t i = 0; i < types::SHADING_COUNT; ++i) {
            if (bgfx::isValid(m_programsDraw[i])) {
                bgfx::destroy(m_programsDraw[i]);
            }
            if (bgfx::isValid(m_programsSimpleDraw[i])) {
                bgfx::destroy(m_programsSimpleDraw[i]);
            }
        }
        if (bgfx::isValid(m_originalSimpleTerrainProgram)) {
            bgfx::destroy(m_originalSimpleTerrainProgram);
        }
        if (bgfx::isValid(m_originalOverlayMaxElevationProgram)) {
            bgfx::destroy(m_originalOverlayMaxElevationProgram);
        }

        if (bgfx::isValid(m_dummySmap)) {
            bgfx::destroy(m_dummySmap);
        }
        if (bgfx::isValid(m_bufferCounter)) {
            bgfx::destroy(m_bufferCounter);
        }
        if (bgfx::isValid(m_bufferCulledSubd)) {
            bgfx::destroy(m_bufferCulledSubd);
        }
        for (int i = 0; i < 2; ++i) {
            if (bgfx::isValid(m_bufferSubd[i])) {
                bgfx::destroy(m_bufferSubd[i]);
            }
        }
        if (bgfx::isValid(m_dispatchIndirect)) {
            bgfx::destroy(m_dispatchIndirect);
        }
        if (bgfx::isValid(m_geometryIndices)) {
            bgfx::destroy(m_geometryIndices);
        }
        if (bgfx::isValid(m_geometryVertices)) {
            bgfx::destroy(m_geometryVertices);
        }
        if (bgfx::isValid(m_instancedGeometryIndices)) {
            bgfx::destroy(m_instancedGeometryIndices);
        }
        if (bgfx::isValid(m_instancedGeometryVertices)) {
            bgfx::destroy(m_instancedGeometryVertices);
        }
        if (bgfx::isValid(m_smapParamsHandle)) {
            bgfx::destroy(m_smapParamsHandle);
        }
        if (bgfx::isValid(m_smapChunkParamsHandle)) {
            bgfx::destroy(m_smapChunkParamsHandle);
        }
        if (bgfx::isValid(m_diffuseUvParamsHandle)) {
            bgfx::destroy(m_diffuseUvParamsHandle);
        }
        if (bgfx::isValid(m_heightfieldDecodeParamsHandle)) {
            bgfx::destroy(m_heightfieldDecodeParamsHandle);
        }
        if (bgfx::isValid(m_programRectWire)) {
            bgfx::destroy(m_programRectWire);
        }
        if (bgfx::isValid(m_programColor)) {
            bgfx::destroy(m_programColor);
        }
        if (bgfx::isValid(m_rectWireVertices)) {
            bgfx::destroy(m_rectWireVertices);
        }
        if (bgfx::isValid(m_rectWireIndices)) {
            bgfx::destroy(m_rectWireIndices);
        }
        if (bgfx::isValid(m_rectParamsBuffer)) {
            bgfx::destroy(m_rectParamsBuffer);
        }
        if (bgfx::isValid(m_rectMaxTexture)) {
            bgfx::destroy(m_rectMaxTexture);
        }
        if (bgfx::isValid(m_rectMaxReadTexture)) {
            bgfx::destroy(m_rectMaxReadTexture);
        }
        if (bgfx::isValid(m_rectMaxSampler)) {
            bgfx::destroy(m_rectMaxSampler);
        }
        if (bgfx::isValid(m_rectMaxParamsHandle)) {
            bgfx::destroy(m_rectMaxParamsHandle);
        }
        if (bgfx::isValid(m_rectViewParamsHandle)) {
            bgfx::destroy(m_rectViewParamsHandle);
        }
        if (bgfx::isValid(m_rectParamsHandle)) {
            bgfx::destroy(m_rectParamsHandle);
        }
        if (bgfx::isValid(m_rectSampleParamsHandle)) {
            bgfx::destroy(m_rectSampleParamsHandle);
        }
        if (bgfx::isValid(m_rectDebugParamsHandle)) {
            bgfx::destroy(m_rectDebugParamsHandle);
        }
    }

    invalidateAllHandles();

    if (m_rectMaxReadPending)
    {
        terrain_internal::stashOverlayReadback(std::move(m_rectMaxReadback), m_rectMaxReadSubmitFrame);
        m_rectMaxReadPending = false;
        m_rectMaxReadCancelPending = false;
        m_rectMaxReadRequested = false;
        m_rectMaxReadSubmitFrame = std::numeric_limits<uint32_t>::max();
    }
    else
    {
        m_rectMaxReadback.clear();
    }

    if (m_dmap) {
        bimg::imageFree(m_dmap);
        m_dmap = nullptr;
    }
}

void TerrainRenderer::deferDestroyTexture(bgfx::TextureHandle handle, uint32_t framesToKeepOld)
{
    if (!bgfx::isValid(handle))
        return;
    // lastFrameId() (not the UINT32_MAX "unknown" sentinel) is a safe base: the
    // old texture was last referenced no later than the previous submitted frame.
    const uint32_t safeFrame = RenderDevice::instance().lastFrameId() + framesToKeepOld;
    RenderDevice::instance().deferUntilFrameRetired(
        [handle]() { if (bgfx::isValid(handle)) bgfx::destroy(handle); },
        safeFrame);
}

bool TerrainRenderer::needsContinuousUpdate() const
{
    // Full (compute) tier continuously refines GPU subdivision each frame unless
    // frozen — it needs to keep rendering. The NoCompute simple grid is static.
    if (!RenderDevice::renderCaps().noCompute() && !m_freeze)
        return true;
    // Any tier needs more frames while a load is still settling (first frame,
    // a pending texture reload, or a GPU SMap result not yet usable).
    return m_heightfieldNeedReload
        || m_diffuseNeedReload
        || !m_firstFrameRendered
        || m_deferSmapUseFrames > 0;
}

void TerrainRenderer::invalidateAllHandles()
{
    m_uniforms.invalidate();

    for (uint32_t i = 0; i < types::TEXTURE_COUNT; ++i) {
        m_textures[i] = BGFX_INVALID_HANDLE;
    }

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_textureCache.clear();
    }

    for (uint32_t i = 0; i < types::SAMPLER_COUNT; ++i) {
        m_samplers[i] = BGFX_INVALID_HANDLE;
    }

    for (uint32_t i = 0; i < types::PROGRAM_COUNT; ++i) {
        m_programsCompute[i] = BGFX_INVALID_HANDLE;
    }

    for (uint32_t i = 0; i < types::SHADING_COUNT; ++i) {
        m_programsDraw[i] = BGFX_INVALID_HANDLE;
        m_programsSimpleDraw[i] = BGFX_INVALID_HANDLE;
    }
    m_originalSimpleTerrainProgram = BGFX_INVALID_HANDLE;
    m_originalOverlayMaxElevationProgram = BGFX_INVALID_HANDLE;
    m_liveTerrainSimpleVertex = LiveShaderSlotState{};
    m_liveTerrainSimpleFragment = LiveShaderSlotState{};
    m_liveOverlayMaxElevationCompute = LiveShaderSlotState{};

    m_dummySmap = BGFX_INVALID_HANDLE;
    m_bufferCounter = BGFX_INVALID_HANDLE;
    m_bufferCulledSubd = BGFX_INVALID_HANDLE;
    m_bufferSubd[0] = BGFX_INVALID_HANDLE;
    m_bufferSubd[1] = BGFX_INVALID_HANDLE;
    m_dispatchIndirect = BGFX_INVALID_HANDLE;
    m_geometryIndices = BGFX_INVALID_HANDLE;
    m_geometryVertices = BGFX_INVALID_HANDLE;
    m_instancedGeometryIndices = BGFX_INVALID_HANDLE;
    m_instancedGeometryVertices = BGFX_INVALID_HANDLE;
    m_smapParamsHandle = BGFX_INVALID_HANDLE;
    m_smapChunkParamsHandle = BGFX_INVALID_HANDLE;
    m_diffuseUvParamsHandle = BGFX_INVALID_HANDLE;
    m_heightfieldDecodeParamsHandle = BGFX_INVALID_HANDLE;
    m_programRectWire = BGFX_INVALID_HANDLE;
    m_programColor = BGFX_INVALID_HANDLE;
    m_colorLayoutReady = false;
    m_rectWireVertices = BGFX_INVALID_HANDLE;
    m_rectWireIndices = BGFX_INVALID_HANDLE;
    m_rectParamsBuffer = BGFX_INVALID_HANDLE;
    m_rectMaxTexture = BGFX_INVALID_HANDLE;
    m_rectMaxReadTexture = BGFX_INVALID_HANDLE;
    m_rectMaxSampler = BGFX_INVALID_HANDLE;
    m_rectMaxParamsHandle = BGFX_INVALID_HANDLE;
    m_rectViewParamsHandle = BGFX_INVALID_HANDLE;
    m_rectParamsHandle = BGFX_INVALID_HANDLE;
    m_rectSampleParamsHandle = BGFX_INVALID_HANDLE;
    m_rectDebugParamsHandle = BGFX_INVALID_HANDLE;
    m_rectBufferCapacity = 0;
    m_rectMaxTextureWidth = 0;
    m_rectMaxTextureCompute = false;
    m_rectComputeDirty = true;
    m_overlayTime = 0.0f;
    m_overlayWorldDirty = false;

    m_resourcesValid = false;
    m_restart = true;
    m_heightfieldReady = false;
    m_overlayDebugAxes = false;
    m_heightValueBias = 0.0f;
    m_heightValueScale = 0.3f;
}

void TerrainRenderer::shutdown()
{
    if (m_textureLoader) {
        m_textureLoader->stop();
    }

    destroyAllResources();

    m_width = 0;
    m_height = 0;
    m_frameBuffer = BGFX_INVALID_HANDLE;
    m_viewId = 0;
    m_firstFrameRendered = false;
    m_heightfieldNeedReload = false;
    m_diffuseNeedReload = false;
    m_heightfieldWidth = 0;
    m_heightfieldHeight = 0;
    m_heightfieldMips = 1;
    m_heightfieldReady = false;
    m_heightValueBias = 0.0f;
    m_heightValueScale = 0.3f;
    m_smapNeedsRegen = false;
    m_pendingHeightfieldGpuDecodes.clear();
    m_heightfieldCpu.clear();
    m_heightfieldCpuWidth = 0;
    m_heightfieldCpuHeight = 0;
    m_overlayRectsScreen.clear();
    m_overlayRectsWorld.clear();
    m_rectComputeDirty = true;
    m_overlayWorldDirty = false;
    m_rectMaxReadback.clear();
    m_rectMaxReadPending = false;
    m_rectMaxReadRequested = false;
    m_rectMaxReadCancelPending = false;
    m_rectMaxReadSubmitFrame = std::numeric_limits<uint32_t>::max();
}

void TerrainRenderer::resize(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return;
    m_width = w;
    m_height = h;
}

bool TerrainRenderer::update(float deltaTime, const float* viewMtx, const float* projMtx)
{
    if (m_width == 0 || m_height == 0 || viewMtx == nullptr || projMtx == nullptr)
        return false;

    m_currentPerfSample = engine::perf::FramePerfSample{};
    const int64_t updateStart = bx::getHPCounter();

    const bool viewChanged = !m_hasViewProj
        || std::memcmp(m_viewMtx, viewMtx, sizeof(m_viewMtx)) != 0
        || std::memcmp(m_projMtx, projMtx, sizeof(m_projMtx)) != 0;
    if (viewChanged)
    {
        std::memcpy(m_viewMtx, viewMtx, sizeof(m_viewMtx));
        std::memcpy(m_projMtx, projMtx, sizeof(m_projMtx));
        m_hasViewProj = true;
    }
    if (!ensureValidResources())
    {
        return false;
    }
    applyPendingLiveShader();

    if (!bgfx::isValid(m_frameBuffer))
    {
        return false;
    }
    {
        engine::perf::ScopeTimer uploadTimer(m_currentPerfSample.uploadMs);
        HeightfieldTextureLoader::LoadRequest request;
        while (m_textureLoader->getLoadedTexture(request)) {
            uploadLoadedTexture(std::move(request));
        }
        processAsyncUploads();
    }

    const uint8_t viewId = m_viewId;

    bgfx::setViewFrameBuffer(viewId, m_frameBuffer);
    bgfx::setViewRect(viewId, 0, 0, uint16_t(m_width), uint16_t(m_height));
    bgfx::setViewTransform(viewId, viewMtx, projMtx);
    bgfx::touch(viewId);

    // Overlay-max readbacks are released by the device's deferred-delete queue
    // (collected inside RenderDevice::endFrame()).

    m_overlayTime += deltaTime;

    processPendingGpuDecodes();

    if (m_heightfieldNeedReload || m_diffuseNeedReload || !m_firstFrameRendered)
    {
        const int64_t t0 = bx::getHPCounter();
        loadTextures();
        const int64_t t1 = bx::getHPCounter();

        m_loadTime = float((t1 - t0) / double(bx::getHPFrequency()) * 1000.0);
        m_currentPerfSample.textureLoadMs = m_loadTime;
        LOG_I("[engine.perf] texture_load_ms={:.3f} heightfield={} diffuse={}",
              m_loadTime,
              m_heightfieldPath,
              m_diffuseTexturePath);
        m_firstFrameRendered = true;
        m_heightfieldNeedReload = false;
        m_diffuseNeedReload   = false;

        LoadTimeRecord rec{};
        rec.loadTimeMs = m_loadTime;
        std::strncpy(rec.heightfieldName, m_heightfieldPath, sizeof(rec.heightfieldName));
        rec.heightfieldName[sizeof(rec.heightfieldName) - 1] = '\0';
        std::strncpy(rec.diffuseName, m_diffuseTexturePath, sizeof(rec.diffuseName));
        rec.diffuseName[sizeof(rec.diffuseName) - 1] = '\0';
        rec.timestamp = t1;

        if (m_loadHistoryCount < MAX_LOAD_HISTORY)
            m_loadHistory[m_loadHistoryCount++] = rec;
        else
        {
            for (int i = 1; i < MAX_LOAD_HISTORY; ++i)
                m_loadHistory[i - 1] = m_loadHistory[i];
            m_loadHistory[MAX_LOAD_HISTORY - 1] = rec;
        }
    }

    if (m_smapNeedsRegen && m_useGpuSmap)
    {
        loadSmapTextureGPU();
        m_smapNeedsRegen = false;
    }
    else if (m_smapNeedsRegen && RenderDevice::renderCaps().noCompute())
    {
        cpuRegenerateSmap();
    }
    m_currentPerfSample.smapMs = std::max(m_cpuSmapGenTime, m_gpuSmapGenTime);

    if (m_deferSmapUseFrames > 0)
        --m_deferSmapUseFrames;

    updateOverlayGpuData();

    {
        engine::perf::ScopeTimer renderTimer(m_currentPerfSample.renderSubmitMs);
        configureUniforms();
        renderTerrain();
        renderAxes();
        renderOverlayRects();
    }

    const int64_t updateEnd = bx::getHPCounter();
    m_currentPerfSample.updateMs = float((updateEnd - updateStart) / double(bx::getHPFrequency()) * 1000.0);

    engine::perf::FramePerfSample avg;
    if (m_perfMonitor.push(m_currentPerfSample, avg))
    {
        LOG_I("[engine.perf] avg update_ms={:.3f} upload_ms={:.3f} render_submit_ms={:.3f} texture_load_ms={:.3f} smap_ms={:.3f}",
              avg.updateMs,
              avg.uploadMs,
              avg.renderSubmitMs,
              avg.textureLoadMs,
              avg.smapMs);
    }

    return true;
}

void TerrainRenderer::renderAxes()
{
    if (!m_overlayDebugAxes)
    {
        return;
    }
    if (!bgfx::isValid(m_programColor) || !m_colorLayoutReady)
    {
        return;
    }

    const uint32_t vertexCount = 18;
    if (bgfx::getAvailTransientVertexBuffer(vertexCount, m_colorLayout) < vertexCount)
    {
        return;
    }
    bgfx::TransientVertexBuffer tvb;
    bgfx::allocTransientVertexBuffer(&tvb, vertexCount, m_colorLayout);

    struct AxisVertex
    {
        float x;
        float y;
        float z;
        uint32_t abgr;
    };

    const float axisLen = 0.35f;
    const float axisX = axisLen * m_terrainAspectRatio;
    const float axisY = axisLen;
    const float axisZ = axisLen * 0.5f;
    const float t = 0.01f;

    AxisVertex* v = reinterpret_cast<AxisVertex*>(tvb.data);
    // X axis (red) quad in Y
    v[0] = {0.0f, -t, 0.0f, 0xff0000ff};
    v[1] = {0.0f,  t, 0.0f, 0xff0000ff};
    v[2] = {axisX,  t, 0.0f, 0xff0000ff};
    v[3] = {0.0f, -t, 0.0f, 0xff0000ff};
    v[4] = {axisX,  t, 0.0f, 0xff0000ff};
    v[5] = {axisX, -t, 0.0f, 0xff0000ff};
    // Y axis (green) quad in X
    v[6] = {-t, 0.0f, 0.0f, 0xff00ff00};
    v[7] = { t, 0.0f, 0.0f, 0xff00ff00};
    v[8] = { t, axisY, 0.0f, 0xff00ff00};
    v[9] = {-t, 0.0f, 0.0f, 0xff00ff00};
    v[10]= { t, axisY, 0.0f, 0xff00ff00};
    v[11]= {-t, axisY, 0.0f, 0xff00ff00};
    // Z axis (blue) quad in X
    v[12]= {-t, 0.0f, 0.0f, 0xffff0000};
    v[13]= { t, 0.0f, 0.0f, 0xffff0000};
    v[14]= { t, 0.0f, axisZ, 0xffff0000};
    v[15]= {-t, 0.0f, 0.0f, 0xffff0000};
    v[16]= { t, 0.0f, axisZ, 0xffff0000};
    v[17]= {-t, 0.0f, axisZ, 0xffff0000};

    float model[16];
    buildModelMatrix(model);
    bgfx::setTransform(model);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setState(BGFX_STATE_WRITE_RGB
        | BGFX_STATE_WRITE_A
        | BGFX_STATE_DEPTH_TEST_LESS);
    bgfx::submit(m_viewId, m_programColor);
}

void TerrainRenderer::setGpuSubdivision(int level) {
    if (level != int(m_uniforms.gpuSubd)) {
        m_restart = true;
        m_uniforms.gpuSubd = float(level);
    }
}

void TerrainRenderer::reloadTextures() {
    m_heightfieldNeedReload = m_diffuseNeedReload = true;
}

void TerrainRenderer::setImageTransform(float rotationDeg, float scaleX, float scaleY)
{
    if (std::fabs(scaleX) < 1.0e-6f)
    {
        scaleX = 1.0f;
    }
    if (std::fabs(scaleY) < 1.0e-6f)
    {
        scaleY = 1.0f;
    }

    const bool sameRotation = std::fabs(m_imageRotation - rotationDeg) < 0.001f;
    const bool sameScaleX = std::fabs(m_imageScaleX - scaleX) < 0.001f;
    const bool sameScaleY = std::fabs(m_imageScaleY - scaleY) < 0.001f;
    if (sameRotation && sameScaleX && sameScaleY)
    {
        return;
    }

    m_imageRotation = rotationDeg;
    m_imageScaleX = scaleX;
    m_imageScaleY = scaleY;
    m_overlayWorldDirty = true;
    m_rectComputeDirty = true;
}

void TerrainRenderer::setHeightPixelSize(float pixelSize)
{
    const float clamped = pixelSize > 0.0f ? pixelSize : 0.0f;
    if (std::fabs(m_heightPixelSize - clamped) < 1.0e-6f)
    {
        return;
    }

    m_heightPixelSize = clamped;
    m_overlayWorldDirty = true;
    m_rectComputeDirty = true;
}

void TerrainRenderer::requestLiveShader(const std::string& slot,
                                        const std::string& binPath,
                                        const std::string& hash)
{
    LiveShaderSlotState* state = nullptr;
    if (slot == "terrain_simple.vertex")
        state = &m_liveTerrainSimpleVertex;
    else if (slot == "terrain_simple.fragment")
        state = &m_liveTerrainSimpleFragment;
    else if (slot == "overlay_max_elevation.compute")
        state = &m_liveOverlayMaxElevationCompute;

    if (!state)
    {
        LOG_E("[live-shader] unsupported slot {}", slot);
        return;
    }

    state->pendingBinPath = binPath;
    state->pendingHash = hash;
    state->pending = true;
    state->revertPending = false;
}

void TerrainRenderer::requestRevertLiveShader(const std::string& slot)
{
    LiveShaderSlotState* state = nullptr;
    if (slot == "terrain_simple.vertex")
        state = &m_liveTerrainSimpleVertex;
    else if (slot == "terrain_simple.fragment")
        state = &m_liveTerrainSimpleFragment;
    else if (slot == "overlay_max_elevation.compute")
        state = &m_liveOverlayMaxElevationCompute;

    if (!state)
    {
        LOG_E("[live-shader] unsupported revert slot {}", slot);
        return;
    }

    state->pending = false;
    state->revertPending = true;
}

bool TerrainRenderer::rebuildLiveTerrainSimpleProgram()
{
    if (!bgfx::isValid(m_originalSimpleTerrainProgram))
    {
        m_originalSimpleTerrainProgram = m_programsSimpleDraw[types::PROGRAM_TERRAIN];
    }

    if (!m_liveTerrainSimpleVertex.active && !m_liveTerrainSimpleFragment.active)
    {
        if (bgfx::isValid(m_programsSimpleDraw[types::PROGRAM_TERRAIN]) &&
            m_programsSimpleDraw[types::PROGRAM_TERRAIN].idx != m_originalSimpleTerrainProgram.idx)
        {
            bgfx::destroy(m_programsSimpleDraw[types::PROGRAM_TERRAIN]);
        }
        m_programsSimpleDraw[types::PROGRAM_TERRAIN] = m_originalSimpleTerrainProgram;
        m_originalSimpleTerrainProgram = BGFX_INVALID_HANDLE;
        return true;
    }

    std::string error;
    bgfx::ShaderHandle vsh = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle fsh = BGFX_INVALID_HANDLE;

    if (m_liveTerrainSimpleVertex.active)
    {
        vsh = loadShaderBinaryFile(m_liveTerrainSimpleVertex.activeBinPath, error);
        if (!bgfx::isValid(vsh))
        {
            m_liveTerrainSimpleVertex.lastError = error;
            return false;
        }
    }
    else
    {
        vsh = loadShader("vs_terrain_simple");
        if (!bgfx::isValid(vsh))
        {
            m_liveTerrainSimpleVertex.lastError = "failed to load stock vertex shader vs_terrain_simple";
            return false;
        }
    }

    if (m_liveTerrainSimpleFragment.active)
    {
        fsh = loadShaderBinaryFile(m_liveTerrainSimpleFragment.activeBinPath, error);
        if (!bgfx::isValid(fsh))
        {
            bgfx::destroy(vsh);
            m_liveTerrainSimpleFragment.lastError = error;
            return false;
        }
    }
    else
    {
        fsh = loadShader("fs_terrain_simple");
        if (!bgfx::isValid(fsh))
        {
            bgfx::destroy(vsh);
            m_liveTerrainSimpleFragment.lastError = "failed to load stock fragment shader fs_terrain_simple";
            return false;
        }
    }

    bgfx::ProgramHandle program = bgfx::createProgram(vsh, fsh, true);
    if (!bgfx::isValid(program))
    {
        m_liveTerrainSimpleVertex.lastError = "bgfx::createProgram failed for live terrain_simple program";
        m_liveTerrainSimpleFragment.lastError = "bgfx::createProgram failed for live terrain_simple program";
        return false;
    }

    if (bgfx::isValid(m_programsSimpleDraw[types::PROGRAM_TERRAIN]) &&
        (!bgfx::isValid(m_originalSimpleTerrainProgram) ||
         m_programsSimpleDraw[types::PROGRAM_TERRAIN].idx != m_originalSimpleTerrainProgram.idx))
    {
        bgfx::destroy(m_programsSimpleDraw[types::PROGRAM_TERRAIN]);
    }
    m_programsSimpleDraw[types::PROGRAM_TERRAIN] = program;
    return true;
}

void TerrainRenderer::applyPendingLiveShader()
{
    auto applyTerrainStage = [this](const char* slot, LiveShaderSlotState& state) {
        if (state.revertPending)
        {
            state.revertPending = false;
            state.active = false;
            state.activeHash.clear();
            state.activeBinPath.clear();
            state.lastError.clear();
            if (rebuildLiveTerrainSimpleProgram())
                LOG_I("[live-shader] reverted {}", slot);
            else
                LOG_E("[live-shader] failed to rebuild terrain program while reverting {}", slot);
        }

        if (!state.pending)
            return;

        state.pending = false;
        state.active = true;
        state.activeHash = state.pendingHash;
        state.activeBinPath = state.pendingBinPath;
        state.lastError.clear();
        if (rebuildLiveTerrainSimpleProgram())
        {
            LOG_I("[live-shader] applied {} hash={}", slot, state.activeHash);
            return;
        }

        state.active = false;
        LOG_E("[live-shader] {}", state.lastError);
        rebuildLiveTerrainSimpleProgram();
    };

    applyTerrainStage("terrain_simple.vertex", m_liveTerrainSimpleVertex);
    applyTerrainStage("terrain_simple.fragment", m_liveTerrainSimpleFragment);

    LiveShaderSlotState& compute = m_liveOverlayMaxElevationCompute;
    if (compute.revertPending)
    {
        compute.revertPending = false;
        if (compute.active)
        {
            if (bgfx::isValid(m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION]))
                bgfx::destroy(m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION]);
            m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION] = m_originalOverlayMaxElevationProgram;
            m_originalOverlayMaxElevationProgram = BGFX_INVALID_HANDLE;
            compute = LiveShaderSlotState{};
            LOG_I("[live-shader] reverted overlay_max_elevation.compute");
        }
    }

    if (compute.pending)
    {
        compute.pending = false;
        compute.lastError.clear();

        if (RenderDevice::renderCaps().noCompute())
        {
            compute.lastError = "compute shader live slot is unavailable in no-compute render tier";
            LOG_E("[live-shader] {}", compute.lastError);
            return;
        }

        std::string error;
        bgfx::ShaderHandle csh = loadShaderBinaryFile(compute.pendingBinPath, error);
        if (!bgfx::isValid(csh))
        {
            compute.lastError = error;
            LOG_E("[live-shader] {}", compute.lastError);
            return;
        }

        bgfx::ProgramHandle program = bgfx::createProgram(csh, true);
        if (!bgfx::isValid(program))
        {
            compute.lastError = "bgfx::createProgram failed for live overlay_max_elevation.compute";
            LOG_E("[live-shader] {}", compute.lastError);
            return;
        }

        if (!compute.active)
            m_originalOverlayMaxElevationProgram = m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION];
        else if (bgfx::isValid(m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION]))
            bgfx::destroy(m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION]);

        m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION] = program;
        compute.active = true;
        compute.activeHash = compute.pendingHash;
        compute.activeBinPath = compute.pendingBinPath;
        LOG_I("[live-shader] applied overlay_max_elevation.compute hash={}", compute.activeHash);
    }
}

nlohmann::json TerrainRenderer::liveShaderSnapshot() const
{
    auto slotJson = [](const char* name, const LiveShaderSlotState& state) {
        return nlohmann::json{
            {"slot", name},
            {"active", state.active},
            {"activeHash", state.activeHash},
            {"activeBinPath", state.activeBinPath},
            {"pending", state.pending},
            {"revertPending", state.revertPending},
            {"lastError", state.lastError}
        };
    };

    nlohmann::json slots = nlohmann::json::array({
        slotJson("terrain_simple.vertex", m_liveTerrainSimpleVertex),
        slotJson("terrain_simple.fragment", m_liveTerrainSimpleFragment),
        slotJson("overlay_max_elevation.compute", m_liveOverlayMaxElevationCompute)
    });

    nlohmann::json activeSlots = nlohmann::json::array();
    std::string firstActiveSlot;
    std::string firstActiveHash;
    std::string firstActiveBinPath;
    for (const auto& slot : slots)
    {
        if (slot.value("active", false))
        {
            activeSlots.push_back(slot["slot"]);
            if (firstActiveSlot.empty())
            {
                firstActiveSlot = slot.value("slot", std::string{});
                firstActiveHash = slot.value("activeHash", std::string{});
                firstActiveBinPath = slot.value("activeBinPath", std::string{});
            }
        }
    }

    const bool pending = m_liveTerrainSimpleVertex.pending ||
                         m_liveTerrainSimpleFragment.pending ||
                         m_liveOverlayMaxElevationCompute.pending;
    const bool revertPending = m_liveTerrainSimpleVertex.revertPending ||
                               m_liveTerrainSimpleFragment.revertPending ||
                               m_liveOverlayMaxElevationCompute.revertPending;

    return {
        {"enabled", true},
        {"supportedSlots", {"terrain_simple.vertex", "terrain_simple.fragment", "overlay_max_elevation.compute"}},
        {"active", !activeSlots.empty()},
        {"activeSlot", firstActiveSlot},
        {"activeHash", firstActiveHash},
        {"activeBinPath", firstActiveBinPath},
        {"activeSlots", activeSlots},
        {"slots", slots},
        {"pending", pending},
        {"revertPending", revertPending},
        {"lastError", ""}
    };
}

float TerrainRenderer::computeWorldHeightUnitScale() const
{
    if (m_heightPixelSize <= 0.0f || m_heightfieldHeight == 0)
    {
        return 0.0f;
    }

    const float panelHeight = m_heightPixelSize * float(m_heightfieldHeight);
    if (panelHeight <= 1.0e-6f)
    {
        return 0.0f;
    }

    return 2.0f / panelHeight;
}

float TerrainRenderer::currentRenderDmapFactor() const
{
    const float unitScale = computeWorldHeightUnitScale();
    if (unitScale > 0.0f && m_heightValueScale > 0.0f)
    {
        return m_heightValueScale * unitScale;
    }
    return m_dmapConfig.scale;
}

float TerrainRenderer::currentRenderDmapBias() const
{
    const float unitScale = computeWorldHeightUnitScale();
    if (unitScale > 0.0f && m_heightValueScale > 0.0f)
    {
        return m_heightValueBias * unitScale;
    }
    return 0.0f;
}

float TerrainRenderer::dmapScale() const
{
    const float bias = currentRenderDmapBias();
    const float scale = currentRenderDmapFactor();
    const float maxHeight = bias + scale;
    return std::max(std::fabs(bias), std::fabs(maxHeight));
}

void TerrainRenderer::buildModelMatrix(float* out) const
{
    float scale[16];
    float rotZ[16];
    float rotX[16];
    float temp[16];

    bx::mtxScale(scale, m_imageScaleX, m_imageScaleY, 1.0f);
    bx::mtxRotateZ(rotZ, bx::toRad(m_imageRotation));
    bx::mtxRotateX(rotX, bx::toRad(-90.0f));

    bx::mtxMul(temp, scale, rotZ);
    bx::mtxMul(out, temp, rotX);
}

void TerrainRenderer::loadPrograms() {
    m_samplers[types::TERRAIN_DMAP_SAMPLER] = bgfx::createUniform("u_DmapSampler", bgfx::UniformType::Sampler);
    m_samplers[types::TERRAIN_SMAP_SAMPLER] = bgfx::createUniform("u_SmapSampler", bgfx::UniformType::Sampler);
    m_samplers[types::TERRAIN_DIFFUSE_SAMPLER] = bgfx::createUniform("u_DiffuseSampler", bgfx::UniformType::Sampler);
    m_samplers[types::HEIGHTFIELD_RAW_SAMPLER] = bgfx::createUniform("u_heightfieldRaw", bgfx::UniformType::Sampler);

    m_uniforms.init();

    m_smapParamsHandle = bgfx::createUniform("u_smapParams", bgfx::UniformType::Vec4);
    m_smapChunkParamsHandle = bgfx::createUniform("u_smapChunkParams", bgfx::UniformType::Vec4);
    m_heightfieldDecodeParamsHandle = bgfx::createUniform("u_heightfieldDecodeParams", bgfx::UniformType::Vec4);
    m_diffuseUvParamsHandle = bgfx::createUniform("u_diffuseUvParams", bgfx::UniformType::Vec4);

    m_rectMaxSampler = bgfx::createUniform("u_rectMaxSampler", bgfx::UniformType::Sampler);
    m_rectMaxParamsHandle = bgfx::createUniform("u_rectMaxParams", bgfx::UniformType::Vec4);
    m_rectViewParamsHandle = bgfx::createUniform("u_rectViewParams", bgfx::UniformType::Vec4);
    m_rectParamsHandle = bgfx::createUniform("u_rectParams", bgfx::UniformType::Vec4);
    m_rectSampleParamsHandle = bgfx::createUniform("u_rectSampleParams", bgfx::UniformType::Vec4);
    m_rectDebugParamsHandle = bgfx::createUniform("u_rectDebugParams", bgfx::UniformType::Vec4);

    const bool noCompute = RenderDevice::renderCaps().noCompute();
    if (noCompute)
    {
        m_programsSimpleDraw[types::PROGRAM_TERRAIN] = loadProgram("vs_terrain_simple", "fs_terrain_simple");
        m_programsSimpleDraw[types::PROGRAM_TERRAIN_NORMAL] = loadProgram("vs_terrain_simple", "fs_terrain_simple_normal");
        for (uint32_t i = 0; i < types::PROGRAM_COUNT; ++i)
        {
            m_programsCompute[i] = BGFX_INVALID_HANDLE;
        }
        m_programsDraw[types::PROGRAM_TERRAIN] = BGFX_INVALID_HANDLE;
        m_programsDraw[types::PROGRAM_TERRAIN_NORMAL] = BGFX_INVALID_HANDLE;
    }
    else
    {
        m_programsDraw[types::PROGRAM_TERRAIN] = loadProgram("vs_terrain_render", "fs_terrain_render");
        m_programsDraw[types::PROGRAM_TERRAIN_NORMAL] = loadProgram("vs_terrain_render", "fs_terrain_render_normal");

        m_programsCompute[types::PROGRAM_SUBD_CS_LOD] = bgfx::createProgram(loadShader("cs_terrain_lod"), true);
        m_programsCompute[types::PROGRAM_UPDATE_INDIRECT] = bgfx::createProgram(loadShader("cs_terrain_update_indirect"), true);
        m_programsCompute[types::PROGRAM_UPDATE_DRAW] = bgfx::createProgram(loadShader("cs_terrain_update_draw"), true);
        m_programsCompute[types::PROGRAM_INIT_INDIRECT] = bgfx::createProgram(loadShader("cs_terrain_init"), true);
        m_programsCompute[types::PROGRAM_GENERATE_SMAP] = bgfx::createProgram(loadShader("cs_generate_smap"), true);
        m_programsCompute[types::PROGRAM_HEIGHTFIELD_MINMAX] = bgfx::createProgram(loadShader("cs_heightfield_minmax"), true);
        m_programsCompute[types::PROGRAM_HEIGHTFIELD_REDUCE] = bgfx::createProgram(loadShader("cs_heightfield_reduce"), true);
        m_programsCompute[types::PROGRAM_HEIGHTFIELD_NORMALIZE] = bgfx::createProgram(loadShader("cs_heightfield_normalize"), true);
        m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION] = bgfx::createProgram(loadShader("cs_overlay_max_elevation"), true);
    }

    if (noCompute)
    {
        m_useGpuSmap = false;
        m_useGpuHeightfieldDecode = false;
    }
    
    m_programRectWire = loadProgram("vs_rect_wire", "fs_rect_wire");
    m_programColor = loadProgram("vs_color", "fs_color");

    m_colorLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true, true)
        .end();
    m_colorLayoutReady = true;
}

void TerrainRenderer::loadTextures() {
    loadDmapTexture();
    if (m_useGpuSmap) {
        loadSmapTextureGPU();
    } else if (RenderDevice::renderCaps().noCompute() && !m_dmapNormalizedCpu.empty()) {
        cpuRegenerateSmap();
    } else {
        loadSmapTexture();
    }
    loadDiffuseTexture();
}

void TerrainRenderer::loadBuffers() {
    const bool noCompute = RenderDevice::renderCaps().noCompute();
    if (noCompute)
    {
        loadSimpleGridBuffers();
    }
    else
    {
        loadSubdivisionBuffers();
        loadGeometryBuffers();
        loadInstancedGeometryBuffers();
    }
    loadOverlayBuffers();
}

void TerrainRenderer::createAtomicCounters() {
    if (RenderDevice::renderCaps().noCompute())
    {
        m_bufferCounter = BGFX_INVALID_HANDLE;
        return;
    }
    m_bufferCounter = bgfx::createDynamicIndexBuffer(3, BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ_WRITE);
}

void TerrainRenderer::loadSmapTexture() {
    int64_t startTime = bx::getHPCounter();

    // Previous CPU-based conversion from non-standard heightfields produced incorrect results.
    // Use a minimal default SMAP to avoid carrying forward the bad conversion logic.
    const bgfx::Memory* mem = bgfx::alloc(4 * sizeof(float));
    float* defaultSlopeData = (float*)mem->data;
    defaultSlopeData[0] = 0.0f;
    defaultSlopeData[1] = 0.0f;
    defaultSlopeData[2] = 0.0f;
    defaultSlopeData[3] = 0.0f;

    bgfx::TextureHandle newSmapTexture = bgfx::createTexture2D(
        1, 1, false, 1, bgfx::TextureFormat::RGBA32F,
        BGFX_TEXTURE_NONE, mem
    );

    deferDestroyTexture(m_textures[types::TEXTURE_SMAP], 5);

    m_textures[types::TEXTURE_SMAP] = newSmapTexture;

    int64_t endTime = bx::getHPCounter();
    m_cpuSmapGenTime = float((endTime - startTime) / double(bx::getHPFrequency()) * 1000.0);
    printf("CPU SMap generation time: %.2f ms\n", m_cpuSmapGenTime);
}

void TerrainRenderer::loadSmapTextureGPU() {
    int64_t startTime = bx::getHPCounter();
    
    if (!bgfx::isValid(m_textures[types::TEXTURE_DMAP]) || m_heightfieldWidth == 0 || m_heightfieldHeight == 0) {
        const bgfx::Memory* mem = bgfx::alloc(4 * sizeof(float));
        float* defaultSlopeData = (float*)mem->data;
        defaultSlopeData[0] = 0.0f;
        defaultSlopeData[1] = 0.0f;
        defaultSlopeData[2] = 0.0f;
        defaultSlopeData[3] = 0.0f;

        bgfx::TextureHandle newSmapTexture = bgfx::createTexture2D(
            1, 1, false, 1, bgfx::TextureFormat::RGBA32F,
            BGFX_TEXTURE_NONE, mem
        );

        deferDestroyTexture(m_textures[types::TEXTURE_SMAP], 5);

        m_textures[types::TEXTURE_SMAP] = newSmapTexture;
        m_gpuSmapGenTime = 0.0f;
        return;
    }

    uint16_t w = m_heightfieldWidth;
    uint16_t h = m_heightfieldHeight;
    int mipcnt = m_heightfieldMips;

    LOG_I("[TerrainRenderer] GPU SMap generation start ({}x{})", w, h);
    LOG_D("[TerrainRenderer] GPU SMap params: dmapFactor={:.6f}, dmapBias={:.6f}, heightMin={:.4f}, heightMax={:.4f}, pixelSize={:.6f}, smapFormat=RGBA32F",
          currentRenderDmapFactor(), currentRenderDmapBias(),
          m_heightValueBias, m_heightValueBias + m_heightValueScale, m_heightPixelSize);

    // Guard: some GL drivers reject RGBA32F+COMPUTE_WRITE and silently deposit
    // GL_INVALID_OPERATION in the context, which is then reported at the next
    // GL_CHECK-wrapped glGenTextures call (ensureOverlayMaxTexture).
    // The analogous guard for R32F already exists in updateOverlayGpuData().
    if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA32F, BGFX_TEXTURE_COMPUTE_WRITE))
    {
        LOG_W("[TerrainRenderer] RGBA32F+COMPUTE_WRITE not supported on this driver, falling back to CPU SMap");
        m_useGpuSmap = false;
        loadSmapTexture();
        return;
    }

    bgfx::TextureHandle newSmapTexture = bgfx::createTexture2D(
        w, h, mipcnt > 1, 1, bgfx::TextureFormat::RGBA32F,
        BGFX_TEXTURE_COMPUTE_WRITE
    );

    float smapParams[4] = { (float)w, (float)h, m_terrainAspectRatio, 0.0f };
    bgfx::setUniform(m_smapParamsHandle, smapParams);
    if (bgfx::isValid(m_smapChunkParamsHandle)) {
        float smapChunkParams[4] = { 0.0f, 0.0f, float(w), float(h) };
        bgfx::setUniform(m_smapChunkParamsHandle, smapChunkParams);
    }

    bgfx::setTexture(0, m_samplers[types::TERRAIN_DMAP_SAMPLER], 
        m_textures[types::TEXTURE_DMAP], 
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);

    if (bgfx::isValid(m_dummySmap)) {
        bgfx::setTexture(1, m_samplers[types::TERRAIN_SMAP_SAMPLER], 
            m_dummySmap, 
            BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC);
    }

    bgfx::setImage(1, newSmapTexture, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA32F);

    uint16_t groupsX = (w + 15) / 16;
    uint16_t groupsY = (h + 15) / 16;

    const uint8_t viewId = m_viewId;
    bgfx::dispatch(viewId, m_programsCompute[types::PROGRAM_GENERATE_SMAP], groupsX, groupsY, 1);

    deferDestroyTexture(m_textures[types::TEXTURE_SMAP], 5);

    m_textures[types::TEXTURE_SMAP] = newSmapTexture;

    m_deferSmapUseFrames = 3;

    int64_t endTime = bx::getHPCounter();
    m_gpuSmapGenTime = float((endTime - startTime) / double(bx::getHPFrequency()) * 1000.0);
    LOG_I("[TerrainRenderer] GPU SMap generation done in {:.2f} ms", m_gpuSmapGenTime);
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

void TerrainRenderer::loadGeometryBuffers() {
    const float halfWidth = 1.0f;
    const float halfHeight = 1.0f;

    const float vertices[] = {
        -halfWidth, -halfHeight, 0.0f, 1.0f,
        +halfWidth, -halfHeight, 0.0f, 1.0f,
        +halfWidth, +halfHeight, 0.0f, 1.0f,
        -halfWidth, +halfHeight, 0.0f, 1.0f,
    };

    const uint32_t indices[] = { 0, 1, 3, 2, 3, 1 };

    m_geometryLayout.begin().add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float).end();

    m_geometryVertices = bgfx::createVertexBuffer(
        bgfx::copy(vertices, sizeof(vertices)),
        m_geometryLayout,
        BGFX_BUFFER_COMPUTE_READ
    );
    
    m_geometryIndices = bgfx::createIndexBuffer(
        bgfx::copy(indices, sizeof(indices)),
        BGFX_BUFFER_COMPUTE_READ | BGFX_BUFFER_INDEX32
    );
}

void TerrainRenderer::loadInstancedGeometryBuffers() {
    const float* vertices;
    const uint32_t* indexes;

    switch (int32_t(m_uniforms.gpuSubd)) {
    case 0:
        m_instancedMeshVertexCount = 3;
        m_instancedMeshPrimitiveCount = 1;
        vertices = tables::s_verticesL0;
        indexes = tables::s_indexesL0;
        break;
    case 1:
        m_instancedMeshVertexCount = 6;
        m_instancedMeshPrimitiveCount = 4;
        vertices = tables::s_verticesL1;
        indexes = tables::s_indexesL1;
        break;
    case 2:
        m_instancedMeshVertexCount = 15;
        m_instancedMeshPrimitiveCount = 16;
        vertices = tables::s_verticesL2;
        indexes = tables::s_indexesL2;
        break;
    default:
        m_instancedMeshVertexCount = 45;
        m_instancedMeshPrimitiveCount = 64;
        vertices = tables::s_verticesL3;
        indexes = tables::s_indexesL3;
        break;
    }

    m_instancedGeometryLayout
        .begin()
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();

    m_instancedGeometryVertices = bgfx::createVertexBuffer(
        bgfx::makeRef(vertices, sizeof(float) * 2 * m_instancedMeshVertexCount),
        m_instancedGeometryLayout
    );

    m_instancedGeometryIndices = bgfx::createIndexBuffer(
        bgfx::makeRef(indexes, sizeof(uint32_t) * m_instancedMeshPrimitiveCount * 3),
        BGFX_BUFFER_INDEX32
    );
}

void TerrainRenderer::loadSubdivisionBuffers() {
    const uint32_t bufferCapacity = 1 << 27;

    m_bufferSubd[types::BUFFER_SUBD] = bgfx::createDynamicIndexBuffer(
        bufferCapacity,
        BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_INDEX32
    );

    m_bufferSubd[types::BUFFER_SUBD + 1] = bgfx::createDynamicIndexBuffer(
        bufferCapacity,
        BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_INDEX32
    );

    m_bufferCulledSubd = bgfx::createDynamicIndexBuffer(
        bufferCapacity,
        BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_INDEX32
    );
}

void TerrainRenderer::configureUniforms() {
    float lodFactor = 2.0f * bx::tan(bx::toRad(m_fovy) / 2.0f)
        / m_width * (1 << int(m_uniforms.gpuSubd))
        * m_primitivePixelLengthTarget;

    m_uniforms.lodFactor = lodFactor;
    m_uniforms.dmapFactor = currentRenderDmapFactor();
    m_uniforms.dmapBias = currentRenderDmapBias();
    // Disable terrain frustum clipping to avoid missing edge patches on some devices.
    m_uniforms.cull = 0.0f;
    m_uniforms.freeze = m_freeze ? 1.0f : 0.0f;
    
    m_uniforms.terrainHalfWidth = m_terrainAspectRatio;
    m_uniforms.terrainHalfHeight = 1.0f;
    
}

void TerrainRenderer::renderTerrain()
{
    if (!m_heightfieldReady
        || !bgfx::isValid(m_textures[types::TEXTURE_DMAP])
        || !bgfx::isValid(m_textures[types::TEXTURE_SMAP]))
    {
        return;
    }

    if (RenderDevice::renderCaps().noCompute())
    {
        renderTerrainSimple();
        return;
    }

    const uint8_t viewId = m_viewId;
    bgfx::touch(viewId);

    float model[16];
    buildModelMatrix(model);

    m_uniforms.submit();

    if (m_restart)
    {
        m_pingPong = 1;

        if (bgfx::isValid(m_instancedGeometryVertices))
        {
            bgfx::destroy(m_instancedGeometryVertices);
            m_instancedGeometryVertices = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(m_instancedGeometryIndices))
        {
            bgfx::destroy(m_instancedGeometryIndices);
            m_instancedGeometryIndices = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(m_bufferSubd[types::BUFFER_SUBD]))
        {
            bgfx::destroy(m_bufferSubd[types::BUFFER_SUBD]);
            m_bufferSubd[types::BUFFER_SUBD] = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(m_bufferSubd[types::BUFFER_SUBD + 1]))
        {
            bgfx::destroy(m_bufferSubd[types::BUFFER_SUBD + 1]);
            m_bufferSubd[types::BUFFER_SUBD + 1] = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(m_bufferCulledSubd))
        {
            bgfx::destroy(m_bufferCulledSubd);
            m_bufferCulledSubd = BGFX_INVALID_HANDLE;
        }

        loadInstancedGeometryBuffers();
        loadSubdivisionBuffers();

        bgfx::setBuffer(1, m_bufferSubd[m_pingPong],   bgfx::Access::ReadWrite);
        bgfx::setBuffer(2, m_bufferCulledSubd,         bgfx::Access::ReadWrite);
        bgfx::setBuffer(3, m_dispatchIndirect,         bgfx::Access::ReadWrite);
        bgfx::setBuffer(4, m_bufferCounter,            bgfx::Access::ReadWrite);
        bgfx::setBuffer(8, m_bufferSubd[1 - m_pingPong], bgfx::Access::ReadWrite);
        bgfx::dispatch(viewId, m_programsCompute[types::PROGRAM_INIT_INDIRECT],
                       1, 1, 1);

        m_restart = false;
    }
    else
    {
        bgfx::setBuffer(3, m_dispatchIndirect, bgfx::Access::ReadWrite);
        bgfx::setBuffer(4, m_bufferCounter,    bgfx::Access::ReadWrite);
        bgfx::dispatch(viewId, m_programsCompute[types::PROGRAM_UPDATE_INDIRECT],
                       1, 1, 1);
    }

    bgfx::setBuffer(1, m_bufferSubd[m_pingPong],        bgfx::Access::ReadWrite);
    bgfx::setBuffer(2, m_bufferCulledSubd,              bgfx::Access::ReadWrite);
    bgfx::setBuffer(4, m_bufferCounter,                 bgfx::Access::ReadWrite);
    bgfx::setBuffer(6, m_geometryVertices,              bgfx::Access::Read);
    bgfx::setBuffer(7, m_geometryIndices,               bgfx::Access::Read);
    bgfx::setBuffer(8, m_bufferSubd[1 - m_pingPong],    bgfx::Access::Read);
    bgfx::setTransform(model);

    bgfx::setTexture(0,
                     m_samplers[types::TERRAIN_DMAP_SAMPLER],
                     m_textures[types::TEXTURE_DMAP],
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);

    m_uniforms.submit();

    bgfx::dispatch(viewId,
                   m_programsCompute[types::PROGRAM_SUBD_CS_LOD],
                   m_dispatchIndirect,
                   1);

    bgfx::setBuffer(3, m_dispatchIndirect, bgfx::Access::ReadWrite);
    bgfx::setBuffer(4, m_bufferCounter,    bgfx::Access::ReadWrite);
    m_uniforms.submit();
    bgfx::dispatch(viewId,
                   m_programsCompute[types::PROGRAM_UPDATE_DRAW],
                   1, 1, 1);

    // DMap
    bgfx::setTexture(0,
                     m_samplers[types::TERRAIN_DMAP_SAMPLER],
                     m_textures[types::TEXTURE_DMAP],
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);

    // SMap
    if (m_deferSmapUseFrames > 0 && bgfx::isValid(m_dummySmap))
    {
        bgfx::setTexture(1,
                         m_samplers[types::TERRAIN_SMAP_SAMPLER],
                         m_dummySmap,
                         BGFX_SAMPLER_MIN_ANISOTROPIC |
                         BGFX_SAMPLER_MAG_ANISOTROPIC);
    }
    else
    {
        bgfx::setTexture(1,
                         m_samplers[types::TERRAIN_SMAP_SAMPLER],
                         m_textures[types::TEXTURE_SMAP],
                         BGFX_SAMPLER_MIN_ANISOTROPIC |
                         BGFX_SAMPLER_MAG_ANISOTROPIC);
    }

    // Diffuse
    if (bgfx::isValid(m_textures[types::TEXTURE_DIFFUSE]))
    {
        uint32_t diffuseSamplerFlags = BGFX_SAMPLER_UVW_MIRROR
            | BGFX_SAMPLER_MIN_ANISOTROPIC
            | BGFX_SAMPLER_MAG_ANISOTROPIC
            | BGFX_SAMPLER_MIP_POINT;

        bgfx::setTexture(5,
                         m_samplers[types::TERRAIN_DIFFUSE_SAMPLER],
                         m_textures[types::TEXTURE_DIFFUSE],
                         diffuseSamplerFlags);
    }

    if (bgfx::isValid(m_diffuseUvParamsHandle))
    {
        const float uvParams[4] = { float(m_diffuseUvMode), 0.0f, 0.0f, 0.0f };
        bgfx::setUniform(m_diffuseUvParamsHandle, uvParams);
    }

    bgfx::setTransform(model);
    bgfx::setVertexBuffer(0, m_instancedGeometryVertices);
    bgfx::setIndexBuffer(m_instancedGeometryIndices);

    bgfx::setBuffer(2, m_bufferCulledSubd,   bgfx::Access::Read);
    bgfx::setBuffer(3, m_geometryVertices,   bgfx::Access::Read);
    bgfx::setBuffer(4, m_geometryIndices,    bgfx::Access::Read);

    bgfx::setState(BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_WRITE_Z
                   | BGFX_STATE_DEPTH_TEST_LESS);

    m_uniforms.submit();

    bgfx::submit(viewId,
                 m_programsDraw[m_shading],
                 m_dispatchIndirect);

    m_pingPong = 1 - m_pingPong;
}

void TerrainRenderer::setRenderTarget(bgfx::ViewId viewId, bgfx::FrameBufferHandle framebuffer)
{
    m_viewId = viewId;
    m_frameBuffer = framebuffer;
    bgfx::setViewMode(m_viewId, bgfx::ViewMode::Sequential);
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

namespace {
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

// NoCompute fallback rendering path (loadSimpleGridBuffers / renderTerrainSimple /
// cpuRegenerateSmap) lives in terrain_renderer_simple.cpp.

nlohmann::json TerrainRenderer::performanceSnapshot() const
{
    auto sampleToJson = [](const engine::perf::FramePerfSample& sample) {
        return nlohmann::json{
            {"updateMs", sample.updateMs},
            {"uploadMs", sample.uploadMs},
            {"renderSubmitMs", sample.renderSubmitMs},
            {"textureLoadMs", sample.textureLoadMs},
            {"smapMs", sample.smapMs}
        };
    };

    return {
        {"available", true},
        {"source", "TerrainRenderer"},
        {"currentFrame", sampleToJson(m_currentPerfSample)},
        {"lastTextureLoadMs", m_loadTime},
        {"lastCpuSmapMs", m_cpuSmapGenTime},
        {"lastGpuSmapMs", m_gpuSmapGenTime},
        {"firstFrameRendered", m_firstFrameRendered}
    };
}

nlohmann::json TerrainRenderer::resourcesSnapshot() const
{
    static const char* kTextureNames[] = {"dmap", "smap", "diffuse"};
    static const char* kDrawProgramNames[] = {"terrainNormal", "terrain"};
    static const char* kComputeProgramNames[] = {
        "subdivisionLod",
        "updateIndirect",
        "initIndirect",
        "updateDraw",
        "generateSmap",
        "heightfieldMinmax",
        "heightfieldReduce",
        "heightfieldNormalize",
        "rectMaxHeight"
    };
    static const char* kSamplerNames[] = {"terrainDmap", "terrainSmap", "terrainDiffuse", "heightfieldRaw"};

    nlohmann::json loadHistory = nlohmann::json::array();
    for (int i = 0; i < m_loadHistoryCount; ++i) {
        loadHistory.push_back({
            {"loadTimeMs", m_loadHistory[i].loadTimeMs},
            {"heightfieldName", m_loadHistory[i].heightfieldName},
            {"diffuseName", m_loadHistory[i].diffuseName},
            {"timestamp", m_loadHistory[i].timestamp}
        });
    }

    nlohmann::json textures = nlohmann::json::array();
    int validTextures = 0;
    for (int i = 0; i < types::TEXTURE_COUNT; ++i) {
        const bool valid = bgfx::isValid(m_textures[i]);
        if (valid)
            ++validTextures;
        textures.push_back({
            {"slot", i},
            {"name", kTextureNames[i]},
            {"valid", valid},
            {"handle", m_textures[i].idx}
        });
    }

    nlohmann::json drawPrograms = nlohmann::json::array();
    int validDrawPrograms = 0;
    for (int i = 0; i < types::SHADING_COUNT; ++i) {
        const bool drawValid = bgfx::isValid(m_programsDraw[i]);
        const bool simpleValid = bgfx::isValid(m_programsSimpleDraw[i]);
        if (drawValid)
            ++validDrawPrograms;
        if (simpleValid)
            ++validDrawPrograms;
        drawPrograms.push_back({
            {"slot", i},
            {"name", kDrawProgramNames[i]},
            {"fullPipelineValid", drawValid},
            {"simplePipelineValid", simpleValid},
            {"fullHandle", m_programsDraw[i].idx},
            {"simpleHandle", m_programsSimpleDraw[i].idx}
        });
    }

    nlohmann::json computePrograms = nlohmann::json::array();
    int validComputePrograms = 0;
    for (int i = 0; i < types::PROGRAM_COUNT; ++i) {
        const bool valid = bgfx::isValid(m_programsCompute[i]);
        if (valid)
            ++validComputePrograms;
        computePrograms.push_back({
            {"slot", i},
            {"name", kComputeProgramNames[i]},
            {"valid", valid},
            {"handle", m_programsCompute[i].idx}
        });
    }

    nlohmann::json samplers = nlohmann::json::array();
    int validSamplers = 0;
    for (int i = 0; i < types::SAMPLER_COUNT; ++i) {
        const bool valid = bgfx::isValid(m_samplers[i]);
        if (valid)
            ++validSamplers;
        samplers.push_back({
            {"slot", i},
            {"name", kSamplerNames[i]},
            {"valid", valid},
            {"handle", m_samplers[i].idx}
        });
    }

    nlohmann::json buffers = {
        {"geometryVertices", bgfx::isValid(m_geometryVertices)},
        {"geometryIndices", bgfx::isValid(m_geometryIndices)},
        {"instancedGeometryVertices", bgfx::isValid(m_instancedGeometryVertices)},
        {"instancedGeometryIndices", bgfx::isValid(m_instancedGeometryIndices)},
        {"dispatchIndirect", bgfx::isValid(m_dispatchIndirect)},
        {"bufferCounter", bgfx::isValid(m_bufferCounter)},
        {"bufferCulledSubd", bgfx::isValid(m_bufferCulledSubd)},
        {"subdivisionPing", bgfx::isValid(m_bufferSubd[0])},
        {"subdivisionPong", bgfx::isValid(m_bufferSubd[1])},
        {"simpleGridVertices", bgfx::isValid(m_simpleGridVertices)},
        {"simpleGridIndices", bgfx::isValid(m_simpleGridIndices)},
        {"rectWireVertices", bgfx::isValid(m_rectWireVertices)},
        {"rectWireIndices", bgfx::isValid(m_rectWireIndices)},
        {"rectParamsBuffer", bgfx::isValid(m_rectParamsBuffer)}
    };

    return {
        {"available", true},
        {"source", "TerrainRenderer"},
        {"width", m_width},
        {"height", m_height},
        {"heightfieldPath", m_heightfieldPath},
        {"diffuseTexturePath", m_diffuseTexturePath},
        {"heightfieldReady", m_heightfieldReady},
        {"heightfieldWidth", m_heightfieldWidth},
        {"heightfieldHeight", m_heightfieldHeight},
        {"heightfieldMips", m_heightfieldMips},
        {"resourcesValid", m_resourcesValid},
        {"bgfxGeneration", m_bgfxGeneration},
        {"terrainAspectRatio", m_terrainAspectRatio},
        {"dmapScale", dmapScale()},
        {"imageRotation", m_imageRotation},
        {"imageScaleX", m_imageScaleX},
        {"imageScaleY", m_imageScaleY},
        {"heightPixelSize", m_heightPixelSize},
        {"viewId", m_viewId},
        {"frameBufferValid", bgfx::isValid(m_frameBuffer)},
        {"validTextures", validTextures},
        {"totalTextures", types::TEXTURE_COUNT},
        {"textures", textures},
        {"validDrawPrograms", validDrawPrograms},
        {"validComputePrograms", validComputePrograms},
        {"validSamplers", validSamplers},
        {"drawPrograms", drawPrograms},
        {"computePrograms", computePrograms},
        {"samplers", samplers},
        {"buffers", buffers},
        {"simpleGridIndexCount", m_simpleGridIndexCount},
        {"instancedMeshVertexCount", m_instancedMeshVertexCount},
        {"instancedMeshPrimitiveCount", m_instancedMeshPrimitiveCount},
        {"wireframe", m_wireframe},
        {"culling", m_cull},
        {"freeze", m_freeze},
        {"gpuSmapEnabled", m_useGpuSmap},
        {"gpuHeightfieldDecodeEnabled", m_useGpuHeightfieldDecode},
        {"liveShader", liveShaderSnapshot()},
        {"overlayRectsScreen", m_overlayRectsScreen.size()},
        {"overlayRectsWorld", m_overlayRectsWorld.size()},
        {"overlayUseScreenSpace", m_overlayUseScreenSpace},
        {"overlayMaxReady", overlayMaxReady()},
        {"loadHistory", loadHistory}
    };
}
