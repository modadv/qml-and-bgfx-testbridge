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
#include <algorithm>
#include <limits>

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
    if (m_presenter.hasInFlightReadbacks())
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

    // The presenter owns the offscreen surface + readback ring; drive the scene
    // (an IRenderContent) through it. It binds the render target and resizes the
    // scene as needed.
    if (!m_presenter.ensureSurface(sz, m_nativeWindowHandle, m_scene))
    {
        finishExternal();
        return;
    }

    m_presenter.processCompletedReadbacks(fbo);
    m_presenter.scheduleReadbacksFromQueue();

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

    // Only stall when the readback ring is actually full. Gating on "any readback
    // in flight" serialised the ring to depth 1: one blit, then two drain frames
    // waiting for it to land, so content reached the FBO at a third of the loop
    // rate. With a free slot we render and blit this frame and let the earlier
    // readbacks retire in parallel.
    if (!m_presenter.hasFreeReadbackSlot())
    {
        const uint32_t currentFrame = m_presenter.endFrame();
        m_presenter.processCompletedReadbacks(fbo);
        m_scene->processOverlayMaxReadback(currentFrame);
        m_presenter.bumpFrameIndex();
        if (shouldContinueRendering())
            update();
        finishExternal();
        return;
    }

    m_presenter.blitForReadback(sz);
    const uint32_t currentFrame = m_presenter.endFrame();

    // Orphaned readbacks are released by the device's deferred-delete queue,
    // collected inside endFrame() above.
    m_presenter.processCompletedReadbacks(fbo);

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

    m_presenter.bumpFrameIndex();
    if (shouldContinueRendering())
        update();
    finishExternal();
}

RenderViewportRenderer::~RenderViewportRenderer()
{
    m_presenter.drainAndRelease();
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
        update();
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
            // The camera moved: request a frame. needsContinuousUpdate() cannot
            // carry this on its own, because render() bakes the view (clearing
            // Camera::viewDirty) before it evaluates the render-on-demand check.
            update();
            event->accept();
            return;
        }
    }

    if (event->buttons() & Qt::MiddleButton || event->buttons() & Qt::RightButton)
    {
        m_scene.handleMouseMove(event);
        update();
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
        update();
        event->accept();
        return;
    }

    event->ignore();
}

void RenderViewportItem::wheelEvent(QWheelEvent* event)
{
    m_scene.handleWheel(event);
    update();
    emit wheelZoomed(event->angleDelta().y());
}
