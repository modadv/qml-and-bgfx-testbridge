#include "render_viewport_item.h"
#include "terrain/render_device.h"
#include "logger.h"

#include <QDir>
#include <QElapsedTimer>
#include <QColor>
#include <QFileInfo>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QMetaObject>
#include <QPointer>
#include <QSize>
#include <QByteArray>
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
#include <deque>
#include <limits>
#include <mutex>

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
} // namespace

static void flipImageVertical(QByteArray& data, int width, int height)
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

//=====================================RenderViewportRenderer======================================

RenderViewportRenderer::RenderViewportRenderer(RenderViewportItem* item)
        : m_item(item)
{
    m_timer.start();
}

QOpenGLFramebufferObject* RenderViewportRenderer::createFramebufferObject(const QSize& size)
{
    QOpenGLFramebufferObjectFormat fmt;
    fmt.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    fmt.setTextureTarget(GL_TEXTURE_2D);
    fmt.setInternalTextureFormat(GL_RGBA8);
    return new QOpenGLFramebufferObject(size, fmt);
}


void RenderViewportRenderer::synchronize(QQuickFramebufferObject* qitem)
{
    QMutexLocker locker(&m_item->m_lock);
    m_scene = &m_item->m_scene;
    auto* item = static_cast<RenderViewportItem*>(qitem);
    if (item && item->window())
    {
        // Ensure native window handle is created
        item->window()->winId();
        m_nativeWindowHandle = reinterpret_cast<void*>(static_cast<uintptr_t>(item->window()->winId()));
        m_scene->setOverlayPixelScale(float(item->window()->devicePixelRatio()));
    }

    auto toLocalPath = [](const QUrl& url) -> QString {
        if (url.isLocalFile())
            return url.toLocalFile();
        const QString asString = QDir::fromNativeSeparators(url.toString());
        if (QFileInfo::exists(asString))
            return asString;
        return asString;
    };

    if (!m_item->heightfieldSource().isEmpty())
    {
        const QUrl url = m_item->heightfieldSource();
        const QString path = toLocalPath(url);
        if (path != m_lastHeightfieldPath)
        {
            m_lastHeightfieldPath = path;
            LOG_I("[RenderViewportItem] heightfieldSource url={} localPath={} exists={}",
                  url.toString().toStdString(),
                  path.toStdString(),
                  QFileInfo::exists(path));
            m_scene->loadHeightfield(path);
        }
    }
    else if (!m_lastHeightfieldPath.isEmpty())
    {
        m_lastHeightfieldPath.clear();
        m_scene->clearHeightfield();
    }

    if (!m_item->diffuseSource().isEmpty())
    {
        const QUrl url = m_item->diffuseSource();
        const QString path = toLocalPath(url);
        if (path != m_lastDiffusePath)
        {
            m_lastDiffusePath = path;
            LOG_I("[RenderViewportItem] diffuseSource url={} localPath={} exists={}",
                  url.toString().toStdString(),
                  path.toStdString(),
                  QFileInfo::exists(path));
            m_scene->loadDiffuse(path);
        }
    }
    else if (!m_lastDiffusePath.isEmpty())
    {
        m_lastDiffusePath.clear();
        m_scene->clearDiffuse();
    }

    if (m_item->m_overlayDirty)
    {
        m_scene->setOverlayRects(m_item->m_pendingOverlayRects);
        m_item->m_overlayDirty = false;
    }

    if (m_item->m_transformDirty)
    {
        m_scene->setImageTransform(
            float(m_item->m_imageRotation),
            float(m_item->m_imageScaleX),
            float(m_item->m_imageScaleY)
        );
        m_item->m_transformDirty = false;
    }

    if (m_item->m_heightPixelSizeDirty)
    {
        m_scene->setHeightPixelSize(float(m_item->m_heightPixelSize));
        m_item->m_heightPixelSizeDirty = false;
    }

    if (m_item->m_pickPending)
    {
        m_pickPending = true;
        m_pickPos = m_item->m_pickPos;
        m_item->m_pickPending = false;
    }

    if (m_item->m_focusPending)
    {
        if (m_scene->focusOverlayRect(m_item->m_focusRectId))
        {
            m_item->m_focusPending = false;
            m_item->m_focusRectId = -1;
        }
    }

    if (m_item->m_liveShaderPending)
    {
        m_scene->requestLiveShader(
            m_item->m_pendingLiveShaderSlot.toStdString(),
            m_item->m_pendingLiveShaderBinPath.toStdString(),
            m_item->m_pendingLiveShaderHash.toStdString());
        m_item->m_pendingLiveShaderSlot.clear();
        m_item->m_pendingLiveShaderBinPath.clear();
        m_item->m_pendingLiveShaderHash.clear();
        m_item->m_liveShaderPending = false;
    }

    if (m_item->m_liveShaderRevertPending)
    {
        m_scene->requestRevertLiveShader(m_item->m_pendingLiveShaderRevertSlot.toStdString());
        m_item->m_pendingLiveShaderRevertSlot.clear();
        m_item->m_liveShaderRevertPending = false;
    }
}

bool RenderViewportRenderer::shouldContinueRendering() const
{
    // Scene is animating or still settling -> keep driving frames.
    if (m_scene && m_scene->needsContinuousUpdate())
        return true;
    // Settle margin after the last animating frame still owes us frames.
    if (m_idleSettleFrames > 0)
        return true;
    // A pick is awaiting an async overlay-max readback to resolve.
    if (m_pickPending)
        return true;
    // Readbacks are still in flight; keep pumping until the FBO is up to date.
    if (!m_pendingReads.empty() || !m_readyForRead.empty())
        return true;
    for (bool inUse : m_readbackInUse)
        if (inUse)
            return true;
    return false;
}

void RenderViewportRenderer::render()
{
    if (!m_scene)
        return;

    QOpenGLFramebufferObject* fbo = framebufferObject();
    if (!fbo)
        return;
    const QSize sz = fbo->size();
    if (sz.width() <= 0 || sz.height() <= 0)
    {
        return;
    }

    QQuickWindow* win = m_item ? m_item->window() : nullptr;
    const bool hasExternal = win != nullptr;
    if (hasExternal)
    {
        win->beginExternalCommands();
    }
    auto finishExternal = [&]() {
        if (!hasExternal)
            return;
        win->endExternalCommands();
        win->resetOpenGLState();
    };

    if (!ensureSurface(sz))
    {
        finishExternal();
        return;
    }

    processCompletedReadbacks();
    scheduleReadbacksFromQueue();

    const float dt = float(m_timer.restart()) / 1000.0f;
    m_scene->update(dt);

    // Render-on-demand: keep a settle margin of frames after the scene last needed
    // continuous updates, so multi-frame content settling (decode/SMap) and the
    // 2-3 frame async readback fully reach the FBO before the loop idles.
    static const int kIdleSettleFrames = 20;
    if (m_scene->needsContinuousUpdate())
        m_idleSettleFrames = kIdleSettleFrames;
    else if (m_idleSettleFrames > 0)
        --m_idleSettleFrames;

    const uint8_t readbackCount = RenderDevice::instance().readbackCount();
    bool readbackInFlight = !m_pendingReads.empty() || !m_readyForRead.empty();
    if (!readbackInFlight)
    {
        for (bool inUse : m_readbackInUse)
        {
            if (inUse)
            {
                readbackInFlight = true;
                break;
            }
        }
    }

    if (readbackInFlight)
    {
        const uint32_t currentFrame = RenderDevice::instance().endFrame();
        m_lastFrameId = currentFrame;
        processCompletedReadbacks();
        if (m_scene)
        {
            m_scene->processOverlayMaxReadback(currentFrame);
        }
        ++m_frameIndex;
        if (shouldContinueRendering())
            update();
        finishExternal();
        return;
    }
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

    const uint32_t currentFrame = RenderDevice::instance().endFrame();
    m_lastFrameId = currentFrame;

    // Orphaned readbacks are released by the device's deferred-delete queue,
    // collected inside endFrame() above.
    processCompletedReadbacks();

    if (m_scene)
    {
        m_scene->processOverlayMaxReadback(currentFrame);
        if (m_pickPending)
        {
            int rectId = -1;
            const bool hit = m_scene->pickOverlayRect(m_pickPos, rectId);
            if (hit)
            {
                QPointer<RenderViewportItem> item = m_item;
                QMetaObject::invokeMethod(m_item, [item, rectId]() {
                    if (item)
                    {
                        emit item->overlayRectClicked(rectId);
                    }
                }, Qt::QueuedConnection);
                m_pickPending = false;
            }
            else if (!m_scene->overlayMaxReady())
            {
                if (!m_scene->hasOverlayRects())
                {
                    m_pickPending = false;
                }
                else
                {
                    m_scene->requestOverlayMaxReadback();
                }
            }
            else
            {
                m_pickPending = false;
            }
        }
    }

    ++m_frameIndex;
    if (shouldContinueRendering())
        update();
    finishExternal();
}

RenderViewportRenderer::~RenderViewportRenderer()
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

bool RenderViewportRenderer::ensureSurface(const QSize& sz)
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
        pd.nwh          = m_nativeWindowHandle;
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
        LOG_I("[RenderViewportItem] bgfx platform data on Windows: nwh={} (forced={})",
              pd.nwh ? "window" : "null",
              useWindowHandle ? "1" : "0");
#endif
        pd.backBuffer   = nullptr;
        pd.backBufferDS = nullptr;
        if (!pd.context && pd.nwh != nullptr)
        {
            LOG_W("[RenderViewportItem] Missing GL context for bgfx PlatformData");
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

    if (m_scene)
    {
        m_scene->setRenderTarget(m_surface.renderViewId, m_surface.framebuffer);
        if (recreated || !m_sceneInited || sizeChanged)
        {
            m_scene->resize(uint32_t(sz.width()), uint32_t(sz.height()));
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

void RenderViewportRenderer::resetReadbackState()
{
    stashPendingReads(m_pendingReads);
    m_readyForRead.clear();
    m_lastFrameId = std::numeric_limits<uint32_t>::max();
    m_frameIndex  = 0;
    m_readbackInUse.fill(false);
    m_nextReadbackIndex = 0;
}

void RenderViewportRenderer::processCompletedReadbacks()
{
    if (m_lastFrameId == std::numeric_limits<uint32_t>::max())
        return;

    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (!ctx)
        return;

    QOpenGLFunctions* gl = ctx->functions();
    if (!gl)
        return;

    QOpenGLFramebufferObject* fbo = framebufferObject();
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

void RenderViewportRenderer::scheduleReadbacksFromQueue()
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

void RenderViewportRenderer::waitForPendingReadbacks()
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


//======================================RenderViewportItem=====================================


RenderViewportItem::RenderViewportItem(QQuickItem* parent)
    : QQuickFramebufferObject(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setFlag(QQuickItem::ItemAcceptsInputMethod, true);
    setFiltersChildMouseEvents(true);
}

QQuickFramebufferObject::Renderer* RenderViewportItem::createRenderer() const
{
    return new RenderViewportRenderer(const_cast<RenderViewportItem*>(this));
}

void RenderViewportItem::requestLiveShader(const QString& slot,
                                           const QString& binPath,
                                           const QString& hash)
{
    QMutexLocker locker(&m_lock);
    m_pendingLiveShaderSlot = slot;
    m_pendingLiveShaderBinPath = binPath;
    m_pendingLiveShaderHash = hash;
    m_liveShaderPending = true;
    update();
}

void RenderViewportItem::requestRevertLiveShader(const QString& slot)
{
    QMutexLocker locker(&m_lock);
    m_pendingLiveShaderRevertSlot = slot;
    m_liveShaderRevertPending = true;
    update();
}

QUrl RenderViewportItem::heightfieldSource() const
{
    return m_heightfieldSource;
}

QUrl RenderViewportItem::diffuseSource() const
{
    return m_diffuseSource;
}

double RenderViewportItem::imageRotation() const
{
    return m_imageRotation;
}

double RenderViewportItem::imageScaleX() const
{
    return m_imageScaleX;
}

double RenderViewportItem::imageScaleY() const
{
    return m_imageScaleY;
}

double RenderViewportItem::heightPixelSize() const
{
    return m_heightPixelSize;
}

void RenderViewportItem::setHeightfieldSource(const QUrl& url)
{
    if (m_heightfieldSource == url)
        return;

    {
        QMutexLocker locker(&m_lock);
        m_heightfieldSource = url;
    }

    emit heightfieldSourceChanged();
    update();
}

void RenderViewportItem::setDiffuseSource(const QUrl& url)
{
    if (m_diffuseSource == url)
        return;

    {
        QMutexLocker locker(&m_lock);
        m_diffuseSource = url;
    }

    emit diffuseSourceChanged();
    update();
}

void RenderViewportItem::setImageRotation(double rotation)
{
    if (qFuzzyCompare(m_imageRotation + 1.0, rotation + 1.0))
    {
        return;
    }

    {
        QMutexLocker locker(&m_lock);
        m_imageRotation = rotation;
        m_transformDirty = true;
    }

    emit imageRotationChanged();
    update();
}

void RenderViewportItem::setImageScaleX(double scale)
{
    if (qFuzzyCompare(m_imageScaleX + 1.0, scale + 1.0))
    {
        return;
    }

    {
        QMutexLocker locker(&m_lock);
        m_imageScaleX = scale;
        m_transformDirty = true;
    }

    emit imageRotationChanged();
    update();
}

void RenderViewportItem::setImageScaleY(double scale)
{
    if (qFuzzyCompare(m_imageScaleY + 1.0, scale + 1.0))
    {
        return;
    }

    {
        QMutexLocker locker(&m_lock);
        m_imageScaleY = scale;
        m_transformDirty = true;
    }

    emit imageRotationChanged();
    update();
}

void RenderViewportItem::setHeightPixelSize(double pixelSize)
{
    const double clamped = pixelSize > 0.0 ? pixelSize : 0.0;
    if (qFuzzyCompare(m_heightPixelSize + 1.0, clamped + 1.0))
    {
        return;
    }

    {
        QMutexLocker locker(&m_lock);
        m_heightPixelSize = clamped;
        m_heightPixelSizeDirty = true;
    }

    emit heightPixelSizeChanged();
    update();
}

void RenderViewportItem::setOverlayRects(const QVariantList& rects)
{
    std::vector<OverlayRect> parsed;
    parsed.reserve(rects.size());

    for (const QVariant& item : rects)
    {
        const QVariantMap map = item.toMap();
        if (map.isEmpty())
        {
            continue;
        }

        OverlayRect rect;
        rect.id = map.value(QStringLiteral("id"), -1).toInt();
        rect.x = map.value(QStringLiteral("x")).toFloat();
        rect.y = map.value(QStringLiteral("y")).toFloat();
        rect.width = map.contains(QStringLiteral("width"))
            ? map.value(QStringLiteral("width")).toFloat()
            : map.value(QStringLiteral("w")).toFloat();
        rect.height = map.contains(QStringLiteral("height"))
            ? map.value(QStringLiteral("height")).toFloat()
            : map.value(QStringLiteral("h")).toFloat();

        QColor color = map.value(QStringLiteral("color")).value<QColor>();
        if (!color.isValid())
        {
            color = QColor(map.value(QStringLiteral("color")).toString());
        }
        if (!color.isValid())
        {
            color = QColor(255, 0, 0, 255);
        }
        rect.color[0] = float(color.redF());
        rect.color[1] = float(color.greenF());
        rect.color[2] = float(color.blueF());
        rect.color[3] = float(color.alphaF());

        rect.lineWidth = map.value(QStringLiteral("lineWidth"), 1.0).toFloat();
        rect.dashLength = map.value(QStringLiteral("dashLength"), 0.0).toFloat();
        rect.dashGap = map.value(QStringLiteral("dashGap"), 0.0).toFloat();
        rect.blinkPeriod = map.value(QStringLiteral("blinkPeriod"), 0.0).toFloat();
        rect.blinkDuty = map.value(QStringLiteral("blinkDuty"), 0.5).toFloat();
        rect.angle = map.value(QStringLiteral("angle"), 0.0).toFloat();
        rect.imageWidth = map.value(QStringLiteral("imageWidth"), 0.0).toFloat();
        rect.imageHeight = map.value(QStringLiteral("imageHeight"), 0.0).toFloat();

        const QString coordType = map.value(QStringLiteral("coordType")).toString().toLower();
        if (coordType == QStringLiteral("pixel_center"))
        {
            rect.coordType = OverlayCoordType::PixelCenter;
        }
        else if (coordType == QStringLiteral("pixel_top_left"))
        {
            rect.coordType = OverlayCoordType::TopLeftPixels;
        }
        else if (coordType == QStringLiteral("normalized_center") || coordType == QStringLiteral("normalized"))
        {
            rect.coordType = OverlayCoordType::NormalizedCenter;
        }

        parsed.push_back(rect);
    }

    {
        QMutexLocker locker(&m_lock);
        m_pendingOverlayRects = std::move(parsed);
        m_overlayDirty = true;
    }

    update();
}

void RenderViewportItem::clearOverlayRects()
{
    {
        QMutexLocker locker(&m_lock);
        m_pendingOverlayRects.clear();
        m_overlayDirty = true;
    }

    update();
}

void RenderViewportItem::setOverlayUseScreenSpace(bool enabled)
{
    QMutexLocker locker(&m_lock);
    m_scene.setOverlayUseScreenSpace(enabled);
    update();
}

void RenderViewportItem::setOverlayDebugAxes(bool enabled)
{
    QMutexLocker locker(&m_lock);
    m_scene.setOverlayDebugAxes(enabled);
    update();
}

void RenderViewportItem::focusOverlayRect(int rectId)
{
    {
        QMutexLocker locker(&m_lock);
        m_focusPending = true;
        m_focusRectId = rectId;
    }
    LOG_D("[RenderViewportItem] Focus overlay rect request id={}", rectId);
    update();
}

void RenderViewportItem::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        {
            QMutexLocker locker(&m_lock);
            m_leftDown = true;
            m_leftDragging = false;
            m_leftPressPos = event->localPos();
        }
        update();
        event->accept();
        return;
    }

    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton)
    {
        m_scene.handleMousePress(event);
        event->accept();
        return;
    }

    event->ignore();
}

void RenderViewportItem::mouseMoveEvent(QMouseEvent* event)
{
    if (event->buttons() & Qt::LeftButton)
    {
        bool startRotate = false;
        {
            QMutexLocker locker(&m_lock);
            if (m_leftDown)
            {
                const qreal dx = event->localPos().x() - m_leftPressPos.x();
                const qreal dy = event->localPos().y() - m_leftPressPos.y();
                if (!m_leftDragging && (dx * dx + dy * dy) >= 9.0)
                {
                    m_leftDragging = true;
                    startRotate = true;
                }
            }
        }

        if (startRotate)
        {
            m_scene.handleMousePress(event);
        }

        if (m_leftDragging)
        {
            m_scene.handleMouseMove(event);
            event->accept();
            return;
        }
    }

    if (event->buttons() & Qt::MiddleButton || event->buttons() & Qt::RightButton)
    {
        m_scene.handleMouseMove(event);
        event->accept();
        return;
    }

    event->ignore();
}

void RenderViewportItem::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        bool wasDragging = false;
        {
            QMutexLocker locker(&m_lock);
            wasDragging = m_leftDragging;
            m_leftDown = false;
            m_leftDragging = false;
        }

        if (wasDragging)
        {
            m_scene.handleMouseRelease(event);
        }
        else
        {
            {
                QMutexLocker locker(&m_lock);
                m_pickPending = true;
                const qreal dpr = window() ? window()->devicePixelRatio() : 1.0;
                m_pickPos = QPointF(event->localPos().x() * dpr, event->localPos().y() * dpr);
            }
            m_scene.requestOverlayMaxReadback();
        }

        update();
        event->accept();
        return;
    }

    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton)
    {
        m_scene.handleMouseRelease(event);
        event->accept();
        return;
    }

    event->ignore();
}

void RenderViewportItem::wheelEvent(QWheelEvent* event)
{
    m_scene.handleWheel(event);
    emit wheelZoomed(event->angleDelta().y());
}
