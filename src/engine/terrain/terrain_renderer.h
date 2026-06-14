#pragma once

#include "terrain_uniforms.h"
#include "terrain_types.h"
#include "heightfield_asset.h"
#include "performance_monitor.h"

#include <bgfx/bgfx.h>
#include <bimg/bimg.h>
#include <bx/file.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <limits>
#include <unordered_map>
#include <chrono>
struct LoadTimeRecord {
    float loadTimeMs;
    char heightfieldName[64];
    char diffuseName[64];
    int64_t timestamp;
};

struct DMap {
    bx::FilePath pathToFile;
    float scale = 0.3f;
};

enum class OverlayCoordType : uint8_t {
    TopLeftPixels = 0,
    NormalizedCenter = 1,
    PixelCenter = 2
};

struct OverlayRect {
    int32_t id = -1;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float lineWidth = 1.0f;
    float dashLength = 0.0f;
    float dashGap = 0.0f;
    float blinkPeriod = 0.0f;
    float blinkDuty = 0.5f;
    float angle = 0.0f;
    OverlayCoordType coordType = OverlayCoordType::TopLeftPixels;
    float imageWidth = 0.0f;
    float imageHeight = 0.0f;
};

struct OverlayQuad {
    int32_t id = -1;
    float x = 0.0f;
    float y = 0.0f;
    float ux = 0.0f;
    float uy = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float lineWidth = 1.0f;
    float dashLength = 0.0f;
    float dashGap = 0.0f;
    float blinkPeriod = 0.0f;
    float blinkDuty = 0.5f;
};

class HeightfieldTextureLoader
{
public:
    enum class DecodeMode : uint8_t
    {
        Int32 = 0,
        Float32 = 1
    };

    struct LoadRequest {
        std::string path;
        int width;
        int height;
        float heightMin = 0.0f;
        float heightMax = 0.0f;
        std::vector<uint16_t> data;
        std::vector<uint8_t> rawData;
        bimg::TextureFormat::Enum rawFormat = bimg::TextureFormat::Count;
        DecodeMode decodeMode = DecodeMode::Int32;
        uint8_t decodeOrder = 0;
        bool rawIsBGRA = true;
        float aspectRatio;
        bool success;
        std::string formatName;
        engine::terrain::HeightSampleType sampleType = engine::terrain::HeightSampleType::NormalizedUnsigned;
    };

    struct LoadParams {
        std::string path;
        bool preferGpuDecode = false;
    };

    void dumpRawData(const uint8_t* imageData, int width, int height, int channels);
    HeightfieldTextureLoader()
        : m_shouldStop(false)
        , m_hasRequest(false)
    {
    }

    ~HeightfieldTextureLoader() {
        stop();
    }

    void loadTexture(const std::string& path, bool preferGpuDecode = false) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingRequest.path = path;
            m_pendingRequest.preferGpuDecode = preferGpuDecode;
            m_hasRequest = true;
        }
        
        // Start the worker on first use.
        if (!m_thread.joinable()) {
            m_thread = std::thread(&HeightfieldTextureLoader::run, this);
        }
        
        m_cv.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_shouldStop = true;
        }
        m_cv.notify_one();
        
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    bool getLoadedTexture(LoadRequest& request) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_loadedRequests.empty()) {
            return false;
        }
        request = std::move(m_loadedRequests.front());
        m_loadedRequests.pop_front();
        return true;
    }

    bool hasLoadedTextures() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_loadedRequests.empty();
    }

private:
    void run() {
        while (true) {
            LoadParams params;
            
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { 
                    return m_shouldStop || m_hasRequest; 
                });
                
                if (m_shouldStop) {
                    break;
                }
                
                if (!m_hasRequest) {
                    continue;
                }
                
                params = m_pendingRequest;
                m_hasRequest = false;
            }

            // Load and decode asset data on the worker thread.
            LoadRequest request = loadImageData(params.path, params.preferGpuDecode);

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_loadedRequests.push_back(request);
            }
        }
    }

    LoadRequest loadImageData(const std::string& path, bool preferGpuDecode);
    bool decodeCustomHeightfield(const uint8_t* imageData, int width, int height,
                               int channels, bimg::TextureFormat::Enum format,
                               std::vector<float>& heightMap);
    bool decodeBGRHeightfield(const uint8_t* imageData, int width, int height, 
                           std::vector<float>& heightMap);
    bool decodeRGBAHeightfield(const uint8_t* imageData, int width, int height,
                            bool isBGRA,
                            std::vector<float>& heightMap);
    void convertToUint16Heightfield(const std::vector<float>& heightMap, 
                                  std::vector<uint16_t>& output,
                                  float& minHeight, float& maxHeight);
    static std::vector<float> smoothHeightfield(const std::vector<float>& heightMap, 
                                               int width, int height, 
                                               int kernelSize = 5);
    
    static std::vector<float> medianFilterHeightfield(const std::vector<float>& heightMap,
                                                     int width, int height,
                                                     int windowSize = 5);
    
    static std::vector<float> bilateralFilterHeightfield(const std::vector<float>& heightMap,
                                                        int width, int height,
                                                        int windowSize = 7,
                                                        float sigmaSpatial = 2.0f,
                                                        float sigmaRange = 100.0f);
        
    static std::vector<float> gaussianBlurHeightfield(const std::vector<float>& heightMap,
                                                     int width, int height,
                                                     int kernelSize = 3);
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    LoadParams m_pendingRequest;
    bool m_hasRequest;
    std::atomic<bool> m_shouldStop;
    std::deque<LoadRequest> m_loadedRequests;
};
class TerrainRenderer {
public:
    enum class DiffuseUvMode : uint8_t {
        None = 0,
        SwapUV = 1,
        RotateCW = 2,
        RotateCCW = 3
    };

    static constexpr int MAX_LOAD_HISTORY = 5;

    TerrainRenderer();
    ~TerrainRenderer();

    bool init(uint32_t width, uint32_t height);
    void shutdown();
    bool update(float deltaTime, const float* viewMtx, const float* projMtx);
    void setRenderTarget(bgfx::ViewId viewId, bgfx::FrameBufferHandle framebuffer);

    void resize(uint32_t w, uint32_t h);
    void setWireframe(bool enabled) { m_wireframe = enabled; }
    void setCulling(bool enabled) { m_cull = enabled; }
    void setFreeze(bool enabled) { m_freeze = enabled; }
    void setPrimitivePixelLength(float length) { m_primitivePixelLengthTarget = length; }

    // True while the terrain must keep producing frames on its own: the Full
    // (compute) tier continuously refines GPU subdivision unless frozen, and any
    // tier needs more frames while a load is still settling. The NoCompute simple
    // grid is static once loaded, so this returns false and lets the viewport go
    // idle until the next input/data change. Drives render-on-demand.
    bool needsContinuousUpdate() const;
    void setShading(int shading) { m_shading = shading; }
    void setGpuSubdivision(int level);

    bool loadHeightfieldFromFile(const char* localPath);
    bool loadDiffuseFromFile(const char* localPath);
    void clearHeightfield();
    void clearDiffuse();
    void reloadTextures();
    void setOverlayRects(const std::vector<OverlayRect>& rects);
    void clearOverlayRects();
    void setOverlayUseScreenSpace(bool enabled);
    void setOverlayPixelScale(float scale);
    void setImageTransform(float rotationDeg, float scaleX, float scaleY);
    void setHeightPixelSize(float pixelSize);
    void requestLiveShader(const std::string& slot,
                           const std::string& binPath,
                           const std::string& hash);
    void requestRevertLiveShader(const std::string& slot);

    float getLoadTime() const { return m_loadTime; }
    float getCpuSmapTime() const { return m_cpuSmapGenTime; }
    float getGpuSmapTime() const { return m_gpuSmapGenTime; }
    void setUseGpuHeightfieldDecode(bool enabled) { m_useGpuHeightfieldDecode = enabled; }
    void setOverlayDebugAxes(bool enabled) { m_overlayDebugAxes = enabled; }
    void requestOverlayMaxReadback();
    bool processOverlayMaxReadback(uint32_t frameId);
    bool overlayMaxReady() const;
    bool getOverlayRectWorldBounds(int rectId,
                                   float& outCenterX,
                                   float& outCenterY,
                                   float& outCenterZ,
                                   float& outWidth,
                                   float& outHeight,
                                   float& outNormalX,
                                   float& outNormalY,
                                   float& outNormalZ) const;
    bool getOverlayRectNearestEdgeTargetYaw(int rectId, float& outYawDeg) const;
    bool getAlgorithmDenseSideTargetYaw(float& outYawDeg, int& outRectId) const;
    bool hasOverlayRects() const;
    int pickOverlayRect(float sx, float sy) const;
    bool isHeightfieldReady() const { return m_heightfieldReady; }
    float terrainAspectRatio() const { return m_terrainAspectRatio; }
    float dmapScale() const;
    float imageScaleX() const { return m_imageScaleX; }
    float imageScaleY() const { return m_imageScaleY; }
    nlohmann::json performanceSnapshot() const;
    nlohmann::json resourcesSnapshot() const;

    void loadPrograms();
    void loadTextures();
    void loadBuffers();
    void createAtomicCounters();

    void loadDmapTexture();
    void loadSmapTexture();
    void loadSmapTextureGPU();
    void loadDiffuseTexture();

    void loadGeometryBuffers();
    void loadInstancedGeometryBuffers();
    void loadSubdivisionBuffers();

    void configureUniforms();
    void renderTerrain();
    void renderOverlayRects();
    void renderAxes();

    bool ensureValidResources();

    void destroyAllResources();

    // Frame budgets handed to deferDestroyTexture (the safe-after-frame offset on
    // RenderDevice's deferred-delete queue). Named so the two distinct retirement
    // policies are explicit rather than scattered literals.
    //   - kTextureRetireFrames: swap-in-place textures (smap, diffuse) whose only
    //     in-flight reader is the draw that just sampled the old handle.
    //   - kDmapRetireFrames: the dmap, which can additionally be referenced by an
    //     in-flight async heightfield decode/readback, so it must live longer.
    static constexpr uint32_t kTextureRetireFrames = 5;
    static constexpr uint32_t kDmapRetireFrames    = 60;
    // Frames to keep sampling the previous smap after queuing a fresh GPU smap,
    // so the generate-smap compute dispatch retires before the terrain draw reads
    // its output. This is a *use* deferral (not a delete deferral), so it is not
    // subsumed by the deferred-delete queue.
    static constexpr int kSmapUseDeferFrames = 3;

    // Schedule the old texture handle for destruction once the GPU has retired
    // `framesToKeepOld` more frames (routed through RenderDevice's deferred-delete
    // queue). Replaces the old backup-array + shared-frame-counter mechanism.
    void deferDestroyTexture(bgfx::TextureHandle handle, uint32_t framesToKeepOld);
    void applyPendingLiveShader();
    nlohmann::json liveShaderSnapshot() const;
    // --- Members (unchanged) ---
    Uniforms m_uniforms;

    bgfx::ProgramHandle m_programsCompute[types::PROGRAM_COUNT];
    bgfx::ProgramHandle m_programsDraw[types::SHADING_COUNT];
    bgfx::TextureHandle m_textures[types::TEXTURE_COUNT];
    bgfx::UniformHandle m_samplers[types::SAMPLER_COUNT];
    bgfx::UniformHandle m_smapParamsHandle;
    bgfx::UniformHandle m_smapChunkParamsHandle;
    bgfx::UniformHandle m_diffuseUvParamsHandle = BGFX_INVALID_HANDLE;
    bgfx::DynamicIndexBufferHandle m_bufferSubd[2];
    bgfx::DynamicIndexBufferHandle m_bufferCulledSubd;
    bgfx::DynamicIndexBufferHandle m_bufferCounter;
    bgfx::IndexBufferHandle        m_geometryIndices;
    bgfx::VertexBufferHandle       m_geometryVertices;
    bgfx::VertexLayout             m_geometryLayout;
    bgfx::IndexBufferHandle        m_instancedGeometryIndices;
    bgfx::VertexBufferHandle       m_instancedGeometryVertices;
    bgfx::VertexLayout             m_instancedGeometryLayout;
    bgfx::IndirectBufferHandle     m_dispatchIndirect;
    bgfx::TextureHandle            m_dummySmap = BGFX_INVALID_HANDLE;
    int                            m_deferSmapUseFrames = 0;

    bimg::ImageContainer* m_dmap = nullptr;

    DMap      m_dmapConfig;
    uint32_t  m_width  = 0;
    uint32_t  m_height = 0;
    bgfx::ViewId            m_viewId = 0;
    bgfx::FrameBufferHandle m_frameBuffer = BGFX_INVALID_HANDLE;
    uint32_t  m_instancedMeshVertexCount = 0;
    uint32_t  m_instancedMeshPrimitiveCount = 0;
    int       m_shading  = 0;
    int       m_pingPong = 0;
    float     m_terrainAspectRatio          = 1.0f;
    float     m_primitivePixelLengthTarget  = 1.0f;
    float     m_fovy                        = 60.0f;
    bool      m_restart                     = true;
    bool      m_wireframe                   = false;
    bool      m_cull                        = true;
    bool      m_freeze                      = false;
    bool      m_useGpuSmap                  = true;
    bool      m_heightfieldNeedReload         = false;
    bool      m_diffuseNeedReload           = false;
    bool      m_smapNeedsRegen              = false;

    int64_t   m_loadStartTime = 0;
    bool      m_firstFrameRendered = false;
    float     m_loadTime          = 0.0f;
    float     m_cpuSmapGenTime    = 0.0f;
    float     m_gpuSmapGenTime    = 0.0f;
    engine::perf::RollingPerformanceMonitor m_perfMonitor;
    engine::perf::FramePerfSample m_currentPerfSample;
    uint64_t m_bgfxGeneration = 0;
    bool m_resourcesValid = false;

    void invalidateAllHandles();
    LoadTimeRecord m_loadHistory[MAX_LOAD_HISTORY];
    int            m_loadHistoryCount = 0;

    char m_heightfieldPath[256];
    char m_diffuseTexturePath[256];
    std::unique_ptr<HeightfieldTextureLoader> m_textureLoader;
    void uploadLoadedTexture(HeightfieldTextureLoader::LoadRequest&& request);
    void processPendingGpuDecodes();
    bool canUseGpuHeightfieldDecode() const;
    bool dispatchGpuHeightfieldDecode(HeightfieldTextureLoader::LoadRequest&& request);
    void updateOverlayGpuData();
    void loadOverlayBuffers();
    bool ensureOverlayRectBuffers(uint16_t rectCount);
    bool ensureOverlayMaxTexture(uint16_t rectCount, bool useCompute, bool needReadback);
    void buildModelMatrix(float* out) const;
    float computeWorldHeightUnitScale() const;
    float currentRenderDmapFactor() const;
    float currentRenderDmapBias() const;

    uint16_t m_heightfieldWidth = 0;
    uint16_t m_heightfieldHeight = 0;
    uint8_t m_heightfieldMips = 1;
    bool m_heightfieldReady = false;
    bool m_useGpuHeightfieldDecode = true;
    std::deque<HeightfieldTextureLoader::LoadRequest> m_pendingHeightfieldGpuDecodes;
    bgfx::UniformHandle m_heightfieldDecodeParamsHandle = BGFX_INVALID_HANDLE;
    std::vector<uint16_t> m_heightfieldCpu;
    uint16_t m_heightfieldCpuWidth = 0;
    uint16_t m_heightfieldCpuHeight = 0;

    bgfx::ProgramHandle m_programRectWire = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_programColor = BGFX_INVALID_HANDLE;

    // NoCompute fallback: simple fixed-grid terrain renderer (no isubd / no compute / no indirect).
    bgfx::ProgramHandle m_programsSimpleDraw[types::SHADING_COUNT] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
    struct LiveShaderSlotState
    {
        bool active = false;
        bool pending = false;
        bool revertPending = false;
        std::string pendingBinPath;
        std::string pendingHash;
        std::string activeBinPath;
        std::string activeHash;
        std::string lastError;
    };

    bgfx::ProgramHandle m_originalSimpleTerrainProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_originalOverlayMaxElevationProgram = BGFX_INVALID_HANDLE;
    LiveShaderSlotState m_liveTerrainSimpleVertex;
    LiveShaderSlotState m_liveTerrainSimpleFragment;
    LiveShaderSlotState m_liveOverlayMaxElevationCompute;
    bool rebuildLiveTerrainSimpleProgram();
    bgfx::VertexBufferHandle m_simpleGridVertices = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  m_simpleGridIndices = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout       m_simpleGridLayout;
    uint32_t                 m_simpleGridIndexCount = 0;
    void loadSimpleGridBuffers();
    void renderTerrainSimple();
    void cpuRegenerateSmap();
    std::vector<float> m_dmapNormalizedCpu; // width*height floats in [0,1] for CPU dmap path
    bgfx::VertexBufferHandle m_rectWireVertices = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle m_rectWireIndices = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle m_rectParamsBuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_rectMaxTexture = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_rectMaxReadTexture = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_rectMaxSampler = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_rectMaxParamsHandle = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_rectViewParamsHandle = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_rectParamsHandle = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_rectSampleParamsHandle = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_rectDebugParamsHandle = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout m_rectParamLayout;
    bgfx::VertexLayout m_rectWireLayout;
    bgfx::VertexLayout m_colorLayout;
    bool m_colorLayoutReady = false;
    std::vector<OverlayRect> m_overlayRectsScreen;
    std::vector<OverlayQuad> m_overlayRectsWorld;
    uint16_t m_rectBufferCapacity = 0;
    uint16_t m_rectMaxTextureWidth = 0;
    bool m_rectMaxTextureCompute = false;
    bool m_rectComputeDirty = false;
    float m_overlayTime = 0.0f;
    float m_viewMtx[16]{};
    float m_projMtx[16]{};
    bool m_hasViewProj = false;
    bool m_overlayDebugAxes = false;
    bool m_overlayWorldDirty = false;
    bool m_overlayUseScreenSpace = true;
    float m_overlayPixelScale = 1.0f;
    std::vector<float> m_rectMaxHeights;
    std::vector<float> m_rectMaxReadback;
    uint32_t m_rectMaxReadFrame = std::numeric_limits<uint32_t>::max();
    uint16_t m_rectMaxReadCount = 0;
    bool m_rectMaxReadPending = false;
    bool m_rectMaxReadRequested = false;
    bool m_rectMaxReadCancelPending = false;
    uint32_t m_rectMaxReadSubmitFrame = std::numeric_limits<uint32_t>::max();
    float m_imageRotation = 0.0f;
    float m_imageScaleX = 1.0f;
    float m_imageScaleY = 1.0f;
    float m_heightPixelSize = 0.0f;
    DiffuseUvMode m_diffuseUvMode = DiffuseUvMode::None;
    float m_heightValueBias = 0.0f;
    float m_heightValueScale = 0.3f;

    // Asset cache and asynchronous upload state.
    
    struct CachedTexture {
        uint16_t width;
        uint16_t height;
        float aspectRatio;
        float heightMin;
        float heightMax;
        std::vector<uint16_t> cpuData;  // CPU-side copy used for fast restore.
        uint64_t lastAccessTime;        // LRU timestamp.
    };
    
    // Keyed by source asset path.
    std::unordered_map<std::string, CachedTexture> m_textureCache;
    size_t m_maxCacheSize = 6;
    mutable std::mutex m_cacheMutex;
    
    struct AsyncUploadTask {
        std::string path;
        std::vector<uint16_t> data;
        int width;
        int height;
        float heightMin;
        float heightMax;
        float aspectRatio;
    };
    std::deque<AsyncUploadTask> m_asyncUploadQueue;
    std::mutex m_uploadMutex;
    
    void pruneCache();
    bool tryLoadFromCache(const std::string& path);
    void addToCache(const std::string& path, const CachedTexture& texture);
    void processAsyncUploads();
};
