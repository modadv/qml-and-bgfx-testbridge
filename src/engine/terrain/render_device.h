#pragma once

#include "render_capabilities.h"

#include <bgfx/bgfx.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_set>

class RenderDevice
{
public:
    static constexpr uint8_t kReadbackBufferCount = 3;

    struct ViewSurface
    {
        ViewSurface();

        uint8_t renderViewId = 0;
        uint8_t blitViewId   = 0;
        uint32_t width       = 0;
        uint32_t height      = 0;
        bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle     colorTex    = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle     depthTex    = BGFX_INVALID_HANDLE;
        std::array<bgfx::TextureHandle, kReadbackBufferCount> readbackTex;
        uint64_t generation = 0;
    };

    static RenderDevice& instance();

    bool acquire(uint32_t minWidth, uint32_t minHeight);
    bool release();

    bool isInitialized() const { return m_initialized.load(); }
    uint64_t generation() const { return m_generation.load(); }
    void setPlatformData(const bgfx::PlatformData& pd);

    bool ensureResolution(uint32_t minWidth, uint32_t minHeight);

    bool createSurface(uint32_t w, uint32_t h, ViewSurface& outSurface);
    bool resizeSurface(uint32_t w, uint32_t h, ViewSurface& surface);
    void destroySurface(ViewSurface& surface);

    uint32_t endFrame();
    uint8_t  readbackCount() const { return kReadbackBufferCount; }
    uint32_t lastFrameId() const { return m_lastFrameId.load(); }

    static const engine::RenderCaps& renderCaps();

private:
    RenderDevice();
    ~RenderDevice();
    
    RenderDevice(const RenderDevice&) = delete;
    RenderDevice& operator=(const RenderDevice&) = delete;

    bool doInit(uint32_t w, uint32_t h);
    void doShutdown();
    bool ensureResolutionInternal(uint32_t w, uint32_t h);

    uint8_t allocateViewId();
    void    freeViewId(uint8_t viewId);
    void    destroySurfaceResources(ViewSurface& surface, bool releaseViewIds);
    bool    createSurfaceResources(uint32_t w, uint32_t h, ViewSurface& surface);

    std::mutex m_mutex;
    std::atomic<bool> m_initialized{false};
    std::atomic<int>  m_refCount{0};
    std::atomic<uint64_t> m_generation{0};
    std::atomic<uint32_t> m_lastFrameId{0};

    uint32_t m_backbufferWidth  = 0;
    uint32_t m_backbufferHeight = 0;
    uint8_t  m_nextViewId       = 0;
    std::unordered_set<uint8_t> m_usedViewIds;
    bgfx::PlatformData m_platformData{};
};
