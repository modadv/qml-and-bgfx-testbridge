#pragma once
#include "render_scene.h"
#include "terrain/render_device.h"

#include <QQuickFramebufferObject>
#include <QUrl>
#include <QMutex>
#include <QElapsedTimer>
#include <QVariant>
#include <QPointF>
#include <QString>
#include <array>
#include <vector>

struct BlitRecord
{
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    uint64_t frameIndex = 0;
    int width = 0;
    int height = 0;
    uint8_t texIndex = 0;
};

struct PendingRead
{
    uint32_t frameId = std::numeric_limits<uint32_t>::max();
    int width = 0;
    int height = 0;
    QByteArray pixels;
    uint8_t texIndex = 0;
};


class RenderViewportItem : public QQuickFramebufferObject
{
    Q_OBJECT

    Q_PROPERTY(QUrl heightfieldSource READ heightfieldSource WRITE setHeightfieldSource NOTIFY heightfieldSourceChanged)
    Q_PROPERTY(QUrl diffuseSource   READ diffuseSource   WRITE setDiffuseSource   NOTIFY diffuseSourceChanged)
    Q_PROPERTY(double imageRotation READ imageRotation WRITE setImageRotation NOTIFY imageRotationChanged)
    Q_PROPERTY(double imageScaleX READ imageScaleX WRITE setImageScaleX NOTIFY imageRotationChanged)
    Q_PROPERTY(double imageScaleY READ imageScaleY WRITE setImageScaleY NOTIFY imageRotationChanged)
    Q_PROPERTY(double heightPixelSize READ heightPixelSize WRITE setHeightPixelSize NOTIFY heightPixelSizeChanged)

public:
    explicit RenderViewportItem(QQuickItem* parent = nullptr);
    ~RenderViewportItem() override = default;

    Renderer* createRenderer() const override;

    QUrl heightfieldSource() const;
    QUrl diffuseSource()   const;
    double imageRotation() const;
    double imageScaleX() const;
    double imageScaleY() const;
    double heightPixelSize() const;

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

public slots:
    void setHeightfieldSource(const QUrl& url);
    void setDiffuseSource(const QUrl& url);
    void setImageRotation(double rotation);
    void setImageScaleX(double scale);
    void setImageScaleY(double scale);
    void setHeightPixelSize(double pixelSize);
    Q_INVOKABLE void setOverlayRects(const QVariantList& rects);
    Q_INVOKABLE void clearOverlayRects();
    Q_INVOKABLE void setOverlayUseScreenSpace(bool enabled);
    Q_INVOKABLE void setOverlayDebugAxes(bool enabled);
    Q_INVOKABLE void focusOverlayRect(int rectId);
    void requestLiveShader(const QString& slot,
                           const QString& binPath,
                           const QString& hash);
    void requestRevertLiveShader(const QString& slot);

signals:
    void heightfieldSourceChanged();
    void diffuseSourceChanged();
    void imageRotationChanged();
    void heightPixelSizeChanged();
    void overlayRectClicked(int rectId);
    void wheelZoomed(int angleDelta);

public:
    // Scene state shared by the GUI and render threads.
    RenderScene m_scene;
    mutable QMutex m_lock;
    std::vector<OverlayRect> m_pendingOverlayRects;
    bool m_overlayDirty = false;
    bool m_transformDirty = false;
    bool m_pickPending = false;
    QPointF m_pickPos;
    bool m_leftDown = false;
    bool m_leftDragging = false;
    QPointF m_leftPressPos;
    bool m_focusPending = false;
    int m_focusRectId = -1;
    bool m_liveShaderPending = false;
    bool m_liveShaderRevertPending = false;
    QString m_pendingLiveShaderSlot;
    QString m_pendingLiveShaderBinPath;
    QString m_pendingLiveShaderHash;
    QString m_pendingLiveShaderRevertSlot;
    double m_imageScaleX = 1.0;
    double m_imageScaleY = 1.0;
    double m_imageRotation = 0.0;
    double m_heightPixelSize = 0.0;
    bool m_heightPixelSizeDirty = true;

private:
    QUrl m_heightfieldSource;
    QUrl m_diffuseSource;
};


class RenderViewportRenderer : public QQuickFramebufferObject::Renderer
{
public:
    explicit RenderViewportRenderer(RenderViewportItem* item);
    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override;

    void synchronize(QQuickFramebufferObject* qitem) override;
    void render() override;
    ~RenderViewportRenderer() override;
    bool ensureSurface(const QSize& sz);
    void resetReadbackState();

private:
    QSize m_lastSize;

    void processCompletedReadbacks();
    void scheduleReadbacksFromQueue();
    void waitForPendingReadbacks();
    // Render-on-demand gate: keep self-scheduling frames only while the scene is
    // animating, a pick is pending, or readbacks are still draining. Otherwise
    // the loop idles until the next input/data change calls update().
    bool shouldContinueRendering() const;

    RenderViewportItem*  m_item        = nullptr;
    RenderScene* m_scene       = nullptr;

    bool m_runtimeInited = false;
    bool m_sceneInited   = false;
    RenderDevice::ViewSurface m_surface;

    QElapsedTimer m_timer;
    uint64_t      m_frameIndex   = 0;

    // Render-on-demand settle margin: frames still to render after the scene last
    // reported needsContinuousUpdate()==true, so content + the async readback
    // pipeline fully drain to the FBO before the loop idles. Reset while the scene
    // animates/settles, counted down otherwise.
    int           m_idleSettleFrames = 0;

    uint32_t m_lastFrameId = std::numeric_limits<uint32_t>::max();

    std::deque<BlitRecord>  m_readyForRead;
    std::deque<PendingRead> m_pendingReads;
    std::array<bool, RenderDevice::kReadbackBufferCount> m_readbackInUse{};
    uint8_t m_nextReadbackIndex = 0;

    bool m_pickPending = false;
    QPointF m_pickPos;

    void* m_nativeWindowHandle = nullptr;
    QString m_lastHeightfieldPath;
    QString m_lastDiffusePath;
};
