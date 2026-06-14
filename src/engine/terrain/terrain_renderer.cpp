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
#include "frame_graph.h"
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

// Canonical, tier-specific render-pass topology for the terrain pipeline. This
// declares the fixed ordered passes and each pass's read/write set so the
// ordering can be validated (producer-before-consumer) and introspected via the
// render.resources snapshot. It mirrors the sequence in update(); it is a
// *description*, not a scheduler — it does not drive bgfx submission.
//
// Resource flow (both tiers): a decode/upload pass produces Dmap; smap is
// produced from Dmap; the overlay-max pass produces OverlayMax from Dmap; the
// terrain pass consumes Dmap+Smap+Diffuse and writes the color/depth target;
// axes and overlay-rects draw into the same target (overlay-rects consumes
// OverlayMax); the present pass blits the color target out for readback.
// Diffuse and the raw Heightfield are external inputs (loaded from disk, never
// written by a pass) and so are exempt from ordering validation.
engine::FramePassList buildTerrainFramePasses(bool noCompute, int renderViewId)
{
    using R = engine::PassResource;
    engine::FramePassList passes;
    if (noCompute)
    {
        passes
            .add("heightfield-upload", -1, uint32_t(R::Heightfield), uint32_t(R::Dmap))
            .add("smap-cpu",           -1, uint32_t(R::Dmap),        uint32_t(R::Smap))
            .add("overlay-max-cpu",    -1, uint32_t(R::Dmap),        uint32_t(R::OverlayMax))
            .add("terrain-simple", renderViewId, R::Dmap | R::Smap | R::Diffuse,
                 R::ColorTarget | R::DepthTarget)
            .add("axes",           renderViewId, 0u, R::ColorTarget | R::DepthTarget)
            .add("overlay-rects",  renderViewId, uint32_t(R::OverlayMax), uint32_t(R::ColorTarget))
            .add("present-blit",   -1, uint32_t(R::ColorTarget), 0u);
    }
    else
    {
        passes
            .add("heightfield-decode", -1, uint32_t(R::Heightfield), uint32_t(R::Dmap))
            .add("smap-generate",      -1, uint32_t(R::Dmap),        uint32_t(R::Smap))
            .add("overlay-max",        -1, uint32_t(R::Dmap),        uint32_t(R::OverlayMax))
            .add("terrain",      renderViewId, R::Dmap | R::Smap | R::Diffuse,
                 R::ColorTarget | R::DepthTarget)
            .add("axes",         renderViewId, 0u, R::ColorTarget | R::DepthTarget)
            .add("overlay-rects",renderViewId, uint32_t(R::OverlayMax), uint32_t(R::ColorTarget))
            .add("present-blit", -1, uint32_t(R::ColorTarget), 0u);
    }
    return passes;
}

} // namespace

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

        // Validate the declarative frame-pass ordering once at init. The
        // topology is static per tier, so an invalid result here would be a
        // programming error in the pass declaration above, not a runtime/data
        // condition — surface it loudly rather than silently.
        {
            std::string fgErr;
            if (!buildTerrainFramePasses(RenderDevice::renderCaps().noCompute(),
                                         int(m_viewId)).validate(fgErr))
            {
                LOG_E("[TerrainRenderer] frame-pass validation failed: {}", fgErr.c_str());
            }
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
        {"framePasses", buildTerrainFramePasses(RenderDevice::renderCaps().noCompute(),
                                                int(m_viewId)).toJson()},
        {"loadHistory", loadHistory}
    };
}
