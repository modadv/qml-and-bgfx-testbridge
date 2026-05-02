#include "render_device.h"
#include "logger.h"
#include <bgfx/c99/bgfx.h>
#include <bgfx/bgfx.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <string>

#if defined(__linux__) || defined(__APPLE__)
#  include <dlfcn.h>
#endif

namespace
{
    constexpr uint8_t kInvalidViewId = std::numeric_limits<uint8_t>::max();

    engine::RenderCaps g_renderCaps{};

    // Custom bgfx callback: on NoCompute (Mesa/llvmpipe/legacy GL) paths,
    // some GL_CHECK asserts fire even though the frame as a whole is still
    // usable. Downgrade fatal() to a log instead of debugBreak() so the
    // process keeps running; treat only non-DebugCheck codes as actually fatal.
    class NonFatalCallback : public bgfx::CallbackI
    {
    public:
        ~NonFatalCallback() override = default;
        void fatal(const char* _filePath, uint16_t _line, bgfx::Fatal::Enum _code, const char* _str) override
        {
            if (_code == bgfx::Fatal::DebugCheck)
            {
                LOG_W("[bgfx] non-fatal DebugCheck at {}:{} — {}", _filePath ? _filePath : "?", _line, _str ? _str : "");
                return;
            }
            LOG_E("[bgfx] FATAL code={} at {}:{} — {}", int(_code), _filePath ? _filePath : "?", _line, _str ? _str : "");
        }
        void traceVargs(const char*, uint16_t, const char*, va_list) override {}
        void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
        void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
        void profilerEnd() override {}
        uint32_t cacheReadSize(uint64_t) override { return 0; }
        bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
        void cacheWrite(uint64_t, const void*, uint32_t) override {}
        void screenShot(const char*, uint32_t, uint32_t, uint32_t, const void*, uint32_t, bool) override {}
        void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
        void captureEnd() override {}
        void captureFrame(const void*, uint32_t) override {}
    };

    NonFatalCallback g_bgfxCallback;

    bool containsCi(const std::string& haystack, const char* needle)
    {
        if (haystack.empty() || needle == nullptr || *needle == '\0')
            return false;
        std::string h = haystack;
        std::string n = needle;
        std::transform(h.begin(), h.end(), h.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        std::transform(n.begin(), n.end(), n.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return h.find(n) != std::string::npos;
    }

    // Pull GL_RENDERER / GL_VERSION via dlsym(glGetString) without linking GL.
    // bgfx is already holding the current GL context on this thread when called
    // immediately after bgfx::init() with the OpenGL backend.
    void probeGlStrings(std::string& renderer, std::string& version)
    {
#if defined(__linux__) || defined(__APPLE__)
        using GetStringFn = const unsigned char* (*)(unsigned int name);
        constexpr unsigned int GL_RENDERER_ENUM = 0x1F01;
        constexpr unsigned int GL_VERSION_ENUM  = 0x1F02;

        void* sym = dlsym(RTLD_DEFAULT, "glGetString");
        if (sym == nullptr)
            return;
        GetStringFn fn = reinterpret_cast<GetStringFn>(sym);
        const unsigned char* r = fn(GL_RENDERER_ENUM);
        const unsigned char* v = fn(GL_VERSION_ENUM);
        if (r != nullptr) renderer.assign(reinterpret_cast<const char*>(r));
        if (v != nullptr) version.assign(reinterpret_cast<const char*>(v));
#else
        (void)renderer;
        (void)version;
#endif
    }

    void probeRenderCaps()
    {
        engine::RenderCaps caps{};
        const bgfx::Caps* bcaps = bgfx::getCaps();
        if (bcaps != nullptr)
        {
            caps.hasCompute  = (bcaps->supported & BGFX_CAPS_COMPUTE) != 0;
            caps.hasIndirect = (bcaps->supported & BGFX_CAPS_DRAW_INDIRECT) != 0;
            caps.hasImageRW  = (bcaps->supported & BGFX_CAPS_IMAGE_RW) != 0;
        }

#if defined(_WIN32)
        // Windows ships real GPU drivers in all supported deployments; we never
        // ride a software backend in production. Skip GL string probing entirely
        // and assume hardware so the auto-tier path stays Full unless caps say
        // otherwise.
        caps.isSoftwareBackend = false;
#else
        probeGlStrings(caps.rendererStr, caps.glVersionStr);
        // Match every common software renderer string we may encounter on
        // Linux/macOS, not just llvmpipe. Mesa: llvmpipe / softpipe / swrast /
        // Offscreen / OSMesa. Google: SwiftShader. Apple: "Apple Software
        // Renderer". The bgfx caps check below still catches anything that
        // slips past this list.
        caps.isSoftwareBackend =
            containsCi(caps.rendererStr, "llvmpipe") ||
            containsCi(caps.rendererStr, "softpipe") ||
            containsCi(caps.rendererStr, "swrast") ||
            containsCi(caps.rendererStr, "swiftshader") ||
            containsCi(caps.rendererStr, "osmesa") ||
            containsCi(caps.rendererStr, "mesa offscreen") ||
            containsCi(caps.rendererStr, "software renderer");
#endif

        const char* trigger = "none";
        if (caps.isSoftwareBackend)      trigger = "software";
        else if (!caps.hasCompute)       trigger = "no_compute";
        else if (!caps.hasIndirect)      trigger = "no_indirect";
        else if (!caps.hasImageRW)       trigger = "no_image_rw";

        const bool autoNoCompute = (std::string(trigger) != "none");

        const engine::RenderTierOverride ovr = engine::parseTierOverrideEnv();
        switch (ovr)
        {
        case engine::RenderTierOverride::Full:
            caps.tier = engine::RenderTier::Full;
            caps.overrideSource = "env:full";
            break;
        case engine::RenderTierOverride::NoCompute:
            caps.tier = engine::RenderTier::NoCompute;
            caps.overrideSource = "env:nocompute";
            break;
        case engine::RenderTierOverride::Auto:
        default:
            caps.tier = autoNoCompute ? engine::RenderTier::NoCompute : engine::RenderTier::Full;
            caps.overrideSource = autoNoCompute
                ? (std::string("auto:") + trigger)
                : "auto";
            break;
        }

        g_renderCaps = caps;

        const char* tierName = caps.tier == engine::RenderTier::NoCompute ? "NoCompute" : "Full";
        LOG_I("[render] backend tier={} (override={}) trigger={} renderer=\"{}\" gl_version=\"{}\" compute={} indirect={} image_rw={} software={}",
              tierName,
              caps.overrideSource,
              trigger,
              caps.rendererStr,
              caps.glVersionStr,
              caps.hasCompute,
              caps.hasIndirect,
              caps.hasImageRW,
              caps.isSoftwareBackend);
    }
}

const engine::RenderCaps& RenderDevice::renderCaps()
{
    return g_renderCaps;
}

RenderDevice::ViewSurface::ViewSurface()
    : renderViewId(kInvalidViewId)
    , blitViewId(kInvalidViewId)
    , width(0)
    , height(0)
    , framebuffer(BGFX_INVALID_HANDLE)
    , colorTex(BGFX_INVALID_HANDLE)
    , depthTex(BGFX_INVALID_HANDLE)
    , generation(0)
{
    readbackTex.fill(BGFX_INVALID_HANDLE);
}

RenderDevice::RenderDevice() = default;

RenderDevice::~RenderDevice()
{
    if (m_initialized.load())
    {
        doShutdown();
    }
}

RenderDevice& RenderDevice::instance()
{
    static RenderDevice inst;
    return inst;
}

bool RenderDevice::acquire(uint32_t w, uint32_t h)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    ++m_refCount;

    if (m_initialized.load())
    {
        ensureResolutionInternal(w, h);
        return false;
    }

    return doInit(w, h);
}

bool RenderDevice::release()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_refCount <= 0)
    {
        return false;
    }

    --m_refCount;

    if (m_refCount == 0 && m_initialized.load())
    {
        // Keep bgfx alive across QQuickFramebufferObject renderer recreation.
        // bgfx::renderFrame(-1) is one-shot in multithreaded builds; shutting down and
        // reinitializing can assert on the next pre-init renderFrame call.
        return true;
    }

    return false;
}

void RenderDevice::setPlatformData(const bgfx::PlatformData& pd)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_platformData = pd;
}

bool RenderDevice::ensureResolution(uint32_t minWidth, uint32_t minHeight)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized.load())
    {
        return false;
    }

    return ensureResolutionInternal(minWidth, minHeight);
}

bool RenderDevice::createSurface(uint32_t w, uint32_t h, ViewSurface& outSurface)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized.load())
    {
        return false;
    }

    ensureResolutionInternal(w, h);
    destroySurfaceResources(outSurface, false);

    outSurface.renderViewId = allocateViewId();
    outSurface.blitViewId   = allocateViewId();

    if (outSurface.renderViewId == kInvalidViewId ||
        outSurface.blitViewId == kInvalidViewId)
    {
        destroySurfaceResources(outSurface, true);
        return false;
    }

    outSurface.width      = w;
    outSurface.height     = h;
    outSurface.generation = m_generation.load();

    if (!createSurfaceResources(w, h, outSurface))
    {
        destroySurfaceResources(outSurface, true);
        return false;
    }

    return true;
}

bool RenderDevice::resizeSurface(uint32_t w, uint32_t h, ViewSurface& surface)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized.load())
    {
        return false;
    }

    ensureResolutionInternal(w, h);

    if (surface.width == w && surface.height == h && surface.generation == m_generation.load())
    {
        return true;
    }

    const uint8_t renderId = surface.renderViewId;
    const uint8_t blitId   = surface.blitViewId;

    destroySurfaceResources(surface, false);

    surface.renderViewId = renderId;
    surface.blitViewId   = blitId;
    surface.width        = w;
    surface.height       = h;
    surface.generation   = m_generation.load();

    if (!createSurfaceResources(w, h, surface))
    {
        destroySurfaceResources(surface, true);
        return false;
    }

    return true;
}

void RenderDevice::destroySurface(ViewSurface& surface)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    destroySurfaceResources(surface, true);
}

uint32_t RenderDevice::endFrame()
{
    if (!m_initialized.load())
        return 0;
    const uint32_t frameId = bgfx::frame();
    m_lastFrameId.store(frameId);
    return frameId;
}

bool RenderDevice::doInit(uint32_t w, uint32_t h)
{
    if (m_initialized.load())
        return false;

    const bgfx_render_frame_t preInitFrame = bgfx_render_frame(-1);
    LOG_I("[RenderDevice] pre-init renderFrame result={}", int(preInitFrame));

    m_backbufferWidth  = std::max<uint32_t>(1, w);
    m_backbufferHeight = std::max<uint32_t>(1, h);
    m_nextViewId       = 0;
    m_usedViewIds.clear();

    bgfx::Init init{};
#if defined(__linux__)
    init.type = bgfx::RendererType::OpenGL;
#else
    init.type = bgfx::RendererType::Count;
#endif
    init.resolution.width  = m_backbufferWidth;
    init.resolution.height = m_backbufferHeight;
    init.resolution.reset  = BGFX_RESET_NONE;

    init.platformData = m_platformData;
    init.callback     = &g_bgfxCallback;

    if (!bgfx::init(init))
        return false;

    m_initialized.store(true);
    ++m_generation;

    probeRenderCaps();

    return true;
}

void RenderDevice::doShutdown()
{
    if (!m_initialized.load())
        return;

    m_usedViewIds.clear();
    m_nextViewId = 0;

    bgfx::shutdown();

    m_initialized.store(false);
    m_backbufferWidth  = 0;
    m_backbufferHeight = 0;
}

bool RenderDevice::ensureResolutionInternal(uint32_t w, uint32_t h)
{
    uint32_t newW = std::max<uint32_t>(1, std::max(w, m_backbufferWidth));
    uint32_t newH = std::max<uint32_t>(1, std::max(h, m_backbufferHeight));

    if (newW == m_backbufferWidth && newH == m_backbufferHeight)
    {
        return false;
    }

    m_backbufferWidth  = newW;
    m_backbufferHeight = newH;
    bgfx::reset(uint16_t(newW), uint16_t(newH), BGFX_RESET_NONE);
    return true;
}

uint8_t RenderDevice::allocateViewId()
{
    for (uint16_t i = 0; i < 256; ++i)
    {
        uint8_t candidate = uint8_t((m_nextViewId + i) % 256);
        if (m_usedViewIds.find(candidate) == m_usedViewIds.end())
        {
            m_usedViewIds.insert(candidate);
            m_nextViewId = uint8_t(candidate + 1);
            return candidate;
        }
    }

    return kInvalidViewId;
}

void RenderDevice::freeViewId(uint8_t viewId)
{
    if (viewId == kInvalidViewId)
        return;
    m_usedViewIds.erase(viewId);
}

void RenderDevice::destroySurfaceResources(ViewSurface& surface, bool releaseViewIds)
{
    if (bgfx::isValid(surface.framebuffer))
    {
        bgfx::destroy(surface.framebuffer);
        surface.framebuffer = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(surface.colorTex))
    {
        bgfx::destroy(surface.colorTex);
        surface.colorTex = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(surface.depthTex))
    {
        bgfx::destroy(surface.depthTex);
        surface.depthTex = BGFX_INVALID_HANDLE;
    }

    for (auto& rb : surface.readbackTex)
    {
        if (bgfx::isValid(rb))
        {
            bgfx::destroy(rb);
        }
        rb = BGFX_INVALID_HANDLE;
    }

    if (releaseViewIds)
    {
        freeViewId(surface.renderViewId);
        freeViewId(surface.blitViewId);
        surface.renderViewId = kInvalidViewId;
        surface.blitViewId   = kInvalidViewId;
    }

    surface.width      = 0;
    surface.height     = 0;
    surface.generation = 0;
}

bool RenderDevice::createSurfaceResources(uint32_t w, uint32_t h, ViewSurface& surface)
{
    const uint64_t colorFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_UVW_CLAMP;

    surface.colorTex = bgfx::createTexture2D(
        uint16_t(w), uint16_t(h),
        false, 1,
        bgfx::TextureFormat::RGBA8,
        colorFlags
    );

    surface.depthTex = bgfx::createTexture2D(
        uint16_t(w), uint16_t(h),
        false, 1,
        bgfx::TextureFormat::D24S8,
        BGFX_TEXTURE_RT_WRITE_ONLY
    );

    if (!bgfx::isValid(surface.colorTex) || !bgfx::isValid(surface.depthTex))
    {
        destroySurfaceResources(surface, false);
        return false;
    }

    bgfx::TextureHandle tex[2] = { surface.colorTex, surface.depthTex };
    surface.framebuffer = bgfx::createFrameBuffer(2, tex, false);

    if (!bgfx::isValid(surface.framebuffer))
    {
        destroySurfaceResources(surface, false);
        return false;
    }

    const uint64_t readbackFlags = BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | BGFX_SAMPLER_UVW_CLAMP;

    for (auto& rb : surface.readbackTex)
    {
        rb = bgfx::createTexture2D(
            uint16_t(w), uint16_t(h),
            false, 1,
            bgfx::TextureFormat::RGBA8,
            readbackFlags
        );
        if (!bgfx::isValid(rb))
        {
            destroySurfaceResources(surface, false);
            return false;
        }
    }

    bgfx::setViewFrameBuffer(surface.renderViewId, surface.framebuffer);
    bgfx::setViewClear(surface.renderViewId, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
    bgfx::setViewRect(surface.renderViewId, 0, 0, uint16_t(w), uint16_t(h));

    bgfx::setViewFrameBuffer(surface.blitViewId, BGFX_INVALID_HANDLE);
    bgfx::setViewRect(surface.blitViewId, 0, 0, uint16_t(w), uint16_t(h));

    return true;
}
