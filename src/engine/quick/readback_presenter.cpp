// readback_presenter.cpp
#include "readback_presenter.h"
#include "terrain/render_device.h"
#include "logger.h"

#include <QByteArray>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#if defined(Q_OS_LINUX) && defined(ENGINE_HAS_QX11INFO)
#include <QX11Info>
#elif defined(Q_OS_LINUX) && defined(ENGINE_HAS_QPA_NATIVE_INTERFACE)
#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h>
#endif
#if defined(Q_OS_LINUX)
#include <GL/glx.h>
#endif

#include <algorithm>
#include <limits>
#include <utility>

namespace {
// Hand in-flight readbacks to the device's fence-driven delete queue: each
// buffer is move-captured (kept alive) until bgfx retires the frame its
// readTexture was issued in, then freed — and drained at shutdown. Replaces the
// old file-static orphan deque that leaked on shutdown and could mis-collect
// across the uint32 frame-counter wraparound.
void stashPendingReads(std::deque<PendingRead>& pending)
{
    while (!pending.empty())
    {
        PendingRead read = std::move(pending.front());
        pending.pop_front();
        const uint32_t safeFrame = read.frameId;
        RenderDevice::instance().deferUntilFrameRetired(
            [kept = std::move(read)]() {}, safeFrame);
    }
}

void flipImageVertical(QByteArray& data, int width, int height)
{
    if (width <= 0 || height <= 1)
        return;

    const int rowBytes = width * 4;
    if (data.size() < rowBytes * height)
        return;

    for (int y = 0; y < height / 2; ++y)
    {
        char* top = data.data() + y * rowBytes;
        char* bottom = data.data() + (height - 1 - y) * rowBytes;
        for (int x = 0; x < rowBytes; ++x)
        {
            std::swap(top[x], bottom[x]);
        }
    }
}
} // namespace

bool ReadbackPresenter::ensureSurface(const QSize& sz, void* nativeWindowHandle, IRenderContent* content)
{
    if (!m_runtimeInited)
    {
        bgfx::PlatformData pd{};
#if defined(Q_OS_LINUX)
#if defined(ENGINE_HAS_QX11INFO)
        pd.ndt          = QX11Info::display();
#elif defined(ENGINE_HAS_QPA_NATIVE_INTERFACE)
        QPlatformNativeInterface* nativeInterface = QGuiApplication::platformNativeInterface();
        pd.ndt = nativeInterface ? nativeInterface->nativeResourceForIntegration("display") : nullptr;
#else
        pd.ndt          = nullptr;
#endif
        pd.context      = reinterpret_cast<void*>(glXGetCurrentContext());
#else
        pd.ndt          = nullptr;
#endif
        pd.nwh          = nativeWindowHandle;
#if !defined(Q_OS_LINUX)
        pd.context      = nullptr;
#endif
#if defined(Q_OS_WIN)
        // Qt (D3D11 RHI/ANGLE) may already own a flip-model swapchain on this HWND.
        // Binding bgfx to the same HWND can fail with DXGI ERROR #297.
        // Default to headless bgfx on Windows to avoid swapchain conflicts.
        const QByteArray forceWindowHandle = qgetenv("TESTBRIDGE_BGFX_USE_WINDOW_HANDLE");
        const bool useWindowHandle = (forceWindowHandle == "1");
        if (!useWindowHandle)
        {
            pd.nwh = nullptr;
        }
        LOG_I("[ReadbackPresenter] bgfx platform data on Windows: nwh={} (forced={})",
              pd.nwh ? "window" : "null",
              useWindowHandle ? "1" : "0");
#endif
        pd.backBuffer   = nullptr;
        pd.backBufferDS = nullptr;
        if (!pd.context && pd.nwh != nullptr)
        {
            LOG_W("[ReadbackPresenter] Missing GL context for bgfx PlatformData");
        }
        RenderDevice::instance().setPlatformData(pd);

        RenderDevice::instance().acquire(uint32_t(sz.width()), uint32_t(sz.height()));
        if (!RenderDevice::instance().isInitialized())
        {
            return false;
        }
        m_runtimeInited = true;
    }

    const uint64_t currentGen = RenderDevice::instance().generation();
    if (m_surface.generation != 0 && m_surface.generation != currentGen)
    {
        waitForPendingReadbacks();
        stashPendingReads(m_pendingReads);
        RenderDevice::instance().destroySurface(m_surface);
        m_sceneInited = false;
        resetReadbackState();
    }

    const bool hasSurface = bgfx::isValid(m_surface.framebuffer) &&
        m_surface.generation == currentGen;

    const bool sizeChanged = m_surface.width != uint32_t(sz.width()) ||
                             m_surface.height != uint32_t(sz.height());

    bool recreated = false;

    if (!hasSurface)
    {
        if (!RenderDevice::instance().createSurface(uint32_t(sz.width()), uint32_t(sz.height()), m_surface))
        {
            return false;
        }
        recreated = true;
    }
    else if (sizeChanged)
    {
        waitForPendingReadbacks();
        stashPendingReads(m_pendingReads);
        if (!RenderDevice::instance().resizeSurface(uint32_t(sz.width()), uint32_t(sz.height()), m_surface))
        {
            return false;
        }
        recreated = true;
    }

    if (recreated)
    {
        resetReadbackState();
    }

    if (content)
    {
        content->setRenderTarget(m_surface.renderViewId, m_surface.framebuffer);
        if (recreated || !m_sceneInited || sizeChanged)
        {
            content->resize(uint32_t(sz.width()), uint32_t(sz.height()));
            m_sceneInited = true;
        }
    }

    // After resize/recreation the Qt FBO texture holds uninitialized GPU memory
    // (glTexImage2D with NULL data). bgfx fills it via async readback over
    // several frames; until then, Qt's scene graph composites the garbage as
    // Clear the freshly-bound FBO once so transient frames show a clean
    // background instead of random video memory. m_lastSize covers the case
    // where Qt recreates the FBO without a bgfx-surface size change.
    if (recreated || sizeChanged || m_lastSize != sz)
    {
        if (QOpenGLContext* ctx = QOpenGLContext::currentContext())
        {
            if (QOpenGLFunctions* gl = ctx->functions())
            {
                GLfloat prevClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                gl->glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);
                gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                gl->glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
            }
        }
    }

    m_lastSize = sz;
    return true;
}

void ReadbackPresenter::resetReadbackState()
{
    stashPendingReads(m_pendingReads);
    m_readyForRead.clear();
    m_lastFrameId = std::numeric_limits<uint32_t>::max();
    m_frameIndex  = 0;
    m_readbackInUse.fill(false);
    m_nextReadbackIndex = 0;
}

void ReadbackPresenter::processCompletedReadbacks(QOpenGLFramebufferObject* fbo)
{
    if (m_lastFrameId == std::numeric_limits<uint32_t>::max())
        return;

    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (!ctx)
        return;

    QOpenGLFunctions* gl = ctx->functions();
    if (!gl)
        return;

    if (!fbo)
        return;

    const QSize fboSize = fbo->size();
    const GLuint targetTexture = fbo->texture();
    if (targetTexture == 0)
        return;

    const bgfx::Caps* caps = bgfx::getCaps();
    const bool needFlip = (caps && !caps->originBottomLeft);

    while (!m_pendingReads.empty() && m_pendingReads.front().frameId <= m_lastFrameId)
    {
        PendingRead ready = std::move(m_pendingReads.front());
        m_pendingReads.pop_front();

        if (ready.texIndex < m_readbackInUse.size())
        {
            m_readbackInUse[ready.texIndex] = false;
        }

        if (ready.width != fboSize.width() || ready.height != fboSize.height())
        {
            continue;
        }

        if (ready.width <= 0 || ready.height <= 0)
        {
            continue;
        }

        const size_t expectedBytes = size_t(ready.width) * size_t(ready.height) * 4u;
        if (expectedBytes == 0 || ready.pixels.size() < int(expectedBytes))
        {
            continue;
        }

        if (needFlip)
        {
            flipImageVertical(ready.pixels, ready.width, ready.height);
        }

        gl->glBindTexture(GL_TEXTURE_2D, targetTexture);
        gl->glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            ready.width,
            ready.height,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            ready.pixels.constData()
        );
    }
}

void ReadbackPresenter::scheduleReadbacksFromQueue()
{
    if (m_frameIndex == 0)
        return;

    while (!m_readyForRead.empty() && (m_frameIndex - m_readyForRead.front().frameIndex) >= 1)
    {
        BlitRecord record = m_readyForRead.front();
        m_readyForRead.pop_front();

        if (record.width <= 0 || record.height <= 0)
        {
            if (record.texIndex < m_readbackInUse.size())
            {
                m_readbackInUse[record.texIndex] = false;
            }
            continue;
        }

        const size_t expectedBytes = size_t(record.width) * size_t(record.height) * 4u;
        if (expectedBytes == 0 || expectedBytes > size_t(std::numeric_limits<int>::max()))
        {
            if (record.texIndex < m_readbackInUse.size())
            {
                m_readbackInUse[record.texIndex] = false;
            }
            continue;
        }

        PendingRead pending;
        pending.width  = record.width;
        pending.height = record.height;
        pending.pixels.resize(int(expectedBytes));
        pending.texIndex = record.texIndex;

        const uint32_t readyFrame = bgfx::readTexture(
            record.handle,
            pending.pixels.data()
        );

        if (readyFrame != std::numeric_limits<uint32_t>::max())
        {
            pending.frameId = readyFrame;
            m_pendingReads.push_back(std::move(pending));
        }
        else if (record.texIndex < m_readbackInUse.size())
        {
            m_readbackInUse[record.texIndex] = false;
        }
    }
}

void ReadbackPresenter::blitForReadback(const QSize& sz)
{
    const uint8_t readbackCount = RenderDevice::instance().readbackCount();
    uint8_t dstIdx = readbackCount;
    for (uint8_t i = 0; i < readbackCount; ++i)
    {
        const uint8_t candidate = uint8_t((m_nextReadbackIndex + i) % readbackCount);
        if (!m_readbackInUse[candidate])
        {
            dstIdx = candidate;
            m_nextReadbackIndex = uint8_t((candidate + 1) % readbackCount);
            break;
        }
    }

    if (dstIdx != readbackCount)
    {
        bgfx::TextureHandle dstHandle = m_surface.readbackTex[dstIdx];
        bgfx::blit(
            m_surface.blitViewId,
            dstHandle,
            0, 0,
            m_surface.colorTex
        );

        BlitRecord blitRec;
        blitRec.handle     = dstHandle;
        blitRec.frameIndex = m_frameIndex;
        blitRec.width      = sz.width();
        blitRec.height     = sz.height();
        blitRec.texIndex   = dstIdx;
        m_readyForRead.push_back(blitRec);
        m_readbackInUse[dstIdx] = true;
    }
}

uint32_t ReadbackPresenter::endFrame()
{
    const uint32_t currentFrame = RenderDevice::instance().endFrame();
    m_lastFrameId = currentFrame;
    return currentFrame;
}

bool ReadbackPresenter::hasInFlightReadbacks() const
{
    if (!m_pendingReads.empty() || !m_readyForRead.empty())
        return true;
    for (bool inUse : m_readbackInUse)
        if (inUse)
            return true;
    return false;
}

void ReadbackPresenter::waitForPendingReadbacks()
{
    uint32_t maxFrameId = 0;
    bool hasPending = false;
    for (const auto& pending : m_pendingReads)
    {
        if (pending.frameId == std::numeric_limits<uint32_t>::max())
            continue;
        maxFrameId = std::max(maxFrameId, pending.frameId);
        hasPending = true;
    }

    if (!hasPending || m_lastFrameId == std::numeric_limits<uint32_t>::max())
        return;

    uint32_t current = m_lastFrameId;
    int guard = 0;
    while (current < maxFrameId && guard < 16)
    {
        current = RenderDevice::instance().endFrame();
        ++guard;
    }
    m_lastFrameId = current;
}

void ReadbackPresenter::drainAndRelease()
{
    waitForPendingReadbacks();
    stashPendingReads(m_pendingReads);
    m_readyForRead.clear();

    if (m_runtimeInited)
    {
        RenderDevice::instance().destroySurface(m_surface);
        RenderDevice::instance().release();
        m_runtimeInited = false;
    }
}
