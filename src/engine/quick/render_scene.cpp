// render_scene.cpp
#include "render_scene.h"
#include "terrain/render_device.h"
#include "logger.h"
#include <QDir>
#include <QUrl>
#include <QFileInfo>
#include <QtMath>
#include <QDebug>

static QString normalizeLocalPath(const QString& in)
{
    if (in.isEmpty())
        return QString();

    QString local = in;
    const QUrl url(local);
    if (url.isValid() && url.isLocalFile())
    {
        local = url.toLocalFile();
    }
    else if (url.isValid() && url.scheme() == QStringLiteral("qrc"))
    {
        const QString qrcPath = url.path();
        QString candidate = qrcPath;
        if (!candidate.isEmpty() && QFileInfo::exists(candidate))
        {
            local = candidate;
            LOG_I("[RenderScene] Resolved qrc path to local file: {} -> {}",
                  in.toStdString(), local.toStdString());
        }
        else if (candidate.startsWith('/') && QFileInfo::exists(candidate.mid(1)))
        {
            local = candidate.mid(1);
            LOG_I("[RenderScene] Resolved qrc path to local file: {} -> {}",
                  in.toStdString(), local.toStdString());
        }
    }
    
    local = QDir::fromNativeSeparators(local);
    
    if (local.isEmpty() || local == "." || local == ".." || local == "/")
    {
        LOG_W("[RenderScene] Invalid path after normalization: {}", in.toStdString());
        return QString();
    }
    
    if (!QFileInfo::exists(local))
    {
        LOG_W("[RenderScene] File does not exist: {}", local.toStdString());
        return QString();
    }
    
    return local;
}

// ---------------- OrbitCamera ----------------

nlohmann::json RenderScene::OrbitCamera::exportConfig() const
{
    nlohmann::json config;
    
    config["yaw"] = m_yaw;
    config["pitch"] = m_pitch;
    config["distance"] = m_distance;
    config["target"] = {
        {"x", m_target.x()},
        {"y", m_target.y()},
        {"z", m_target.z()}
    };
    config["fovY"] = m_fovY;
    
    return config;
}

void RenderScene::OrbitCamera::loadConfig(const nlohmann::json& config)
{
    if (config.is_null() || !config.is_object()) {
        LOG_W("[OrbitCamera] Invalid config, using defaults");
        return;
    }
    
    try {
        auto oldCallback = m_onConfigChanged;
        m_onConfigChanged = nullptr;
        
        m_yaw = config.value("yaw", -90.0f);
        m_pitch = config.value("pitch", 20.0f);
        m_distance = config.value("distance", 3.0f);
        m_fovY = config.value("fovY", 60.0f);
        
        if (config.contains("target") && config["target"].is_object()) {
            const auto& t = config["target"];
            m_target.setX(t.value("x", 0.0f));
            m_target.setY(t.value("y", 0.0f));
            m_target.setZ(t.value("z", 0.0f));
        }
        
        m_pitch = qBound(0.0f, m_pitch, 89.0f);
        m_distance = qBound(0.2f, m_distance, 250.0f);
        
        m_viewDirty = true;
        m_projDirty = true;
        
        m_onConfigChanged = oldCallback;
        
        LOG_I("[OrbitCamera] Config loaded: yaw={}, pitch={}, distance={}", 
              m_yaw, m_pitch, m_distance);
    }
    catch (const nlohmann::json::exception& e) {
        LOG_E("[OrbitCamera] Failed to parse config: {}", e.what());
    }
}

void RenderScene::OrbitCamera::notifyConfigChanged()
{
    logState("changed");
    if (m_onConfigChanged) {
        m_onConfigChanged();
    }
}

void RenderScene::OrbitCamera::logState(const char* reason) const
{
    LOG_I("[OrbitCamera] {} yaw={:.3f}, pitch={:.3f}, distance={:.3f}, target=({:.3f}, {:.3f}, {:.3f})",
          reason ? reason : "state",
          m_yaw, m_pitch, m_distance,
          m_target.x(), m_target.y(), m_target.z());
}

// ---------------- OrbitCamera ----------------

void RenderScene::OrbitCamera::resize(uint32_t w, uint32_t h)
{
    m_viewport = QSize(int(qMax<uint32_t>(1, w)), int(qMax<uint32_t>(1, h)));
    m_projDirty = true;
}

void RenderScene::OrbitCamera::beginRotate(const QPointF& pos)
{
    m_lastPos = pos;
    // Force orbit pivot to model center when left-drag rotates the view.
    m_target = QVector3D(0.0f, 0.0f, 0.0f);
    m_viewDirty = true;
    m_rotating = true;
}

void RenderScene::OrbitCamera::rotateTo(const QPointF& pos)
{
    if (!m_rotating)
        return;

    const QPointF delta = pos - m_lastPos;
    m_lastPos = pos;

    constexpr float rotateSpeed = 0.2f;
    m_yaw -= float(delta.x()) * rotateSpeed;
    m_pitch += float(delta.y()) * rotateSpeed;
    // Lower bound is 0 (not -89) so the user cannot rotate the camera under
    // the model and look at its bare/back side. Upper bound 89 preserves
    // straight-down top view.
    m_pitch = qBound(0.0f, m_pitch, 89.0f);

    m_viewDirty = true;
    notifyConfigChanged();
}

void RenderScene::OrbitCamera::endRotate()
{
    m_rotating = false;
}

void RenderScene::OrbitCamera::beginPan(const QPointF& pos)
{
    m_lastPanPos = pos;
    m_panning = true;
}

void RenderScene::OrbitCamera::panTo(const QPointF& pos)
{
    if (!m_panning)
        return;

    const QPointF delta = pos - m_lastPanPos;
    m_lastPanPos = pos;

    const float w = float(m_viewport.width());
    const float h = float(m_viewport.height());
    if (w <= 0.0f || h <= 0.0f)
        return;

    const float aspect = h > 0.0f ? (w / h) : 1.0f;
    const float fovRad = qDegreesToRadians(m_fovY);
    const float viewH = 2.0f * std::tan(fovRad * 0.5f) * m_distance;
    const float viewW = viewH * aspect;

    const float dx = float(delta.x()) / w * viewW;
    const float dy = float(delta.y()) / h * viewH;

    const float yawRad = qDegreesToRadians(m_yaw);
    const float pitchRad = qDegreesToRadians(m_pitch);
    QVector3D forward(
        std::sin(yawRad) * std::cos(pitchRad),
        std::sin(pitchRad),
        std::cos(yawRad) * std::cos(pitchRad)
    );
    forward.normalize();

    const QVector3D worldUp(0.0f, 1.0f, 0.0f);
    QVector3D right = QVector3D::crossProduct(forward, worldUp).normalized();
    QVector3D up = QVector3D::crossProduct(right, forward).normalized();

    m_target += (-dx) * right + (-dy) * up;
    m_viewDirty = true;
    notifyConfigChanged();
}

void RenderScene::OrbitCamera::endPan()
{
    m_panning = false;
}

void RenderScene::OrbitCamera::beginZoomDrag(const QPointF& pos)
{
    m_lastZoomPos = pos;
    m_zooming = true;
}

void RenderScene::OrbitCamera::zoomDragTo(const QPointF& pos)
{
    if (!m_zooming)
        return;

    const QPointF delta = pos - m_lastZoomPos;
    m_lastZoomPos = pos;

    constexpr float zoomSpeed = 0.01f;
    m_distance = qBound(0.2f, m_distance + float(delta.y()) * zoomSpeed, 250.0f);
    m_viewDirty = true;
    notifyConfigChanged();
}

void RenderScene::OrbitCamera::endZoomDrag()
{
    m_zooming = false;
}

void RenderScene::OrbitCamera::zoom(float delta)
{
    constexpr float zoomSpeed = 0.0015f;
    m_distance = qBound(0.2f, m_distance - delta * zoomSpeed, 250.0f);
    m_viewDirty = true;
    notifyConfigChanged();
}

void RenderScene::OrbitCamera::setDistance(float value, bool notify)
{
    const float clamped = qBound(0.2f, value, 250.0f);
    if (qFuzzyCompare(m_distance, clamped))
        return;
    m_distance = clamped;
    m_viewDirty = true;
    if (notify)
    {
        notifyConfigChanged();
    }
}

float RenderScene::OrbitCamera::computeFitDistance(float halfX, float halfY, float halfZ, float fillRatio) const
{
    const float safeFill = qBound(0.5f, fillRatio, 0.98f);
    const float aspect = m_viewport.height() > 0
        ? float(m_viewport.width()) / float(m_viewport.height())
        : 1.0f;
    const float fovYRad = qDegreesToRadians(m_fovY);
    const float tanHalfY = std::tan(fovYRad * 0.5f);
    const float tanHalfX = tanHalfY * qMax(0.01f, aspect);

    const float yawRad = qDegreesToRadians(m_yaw);
    const float pitchRad = qDegreesToRadians(m_pitch);
    QVector3D forward(
        std::sin(yawRad) * std::cos(pitchRad),
        std::sin(pitchRad),
        std::cos(yawRad) * std::cos(pitchRad)
    );
    forward.normalize();
    const QVector3D worldUp(0.0f, 1.0f, 0.0f);
    QVector3D right = QVector3D::crossProduct(forward, worldUp).normalized();
    QVector3D up = QVector3D::crossProduct(right, forward).normalized();

    const float halfWidthCam =
        std::fabs(right.x()) * halfX +
        std::fabs(right.y()) * halfY +
        std::fabs(right.z()) * halfZ;
    const float halfHeightCam =
        std::fabs(up.x()) * halfX +
        std::fabs(up.y()) * halfY +
        std::fabs(up.z()) * halfZ;
    const float halfDepthCam =
        std::fabs(forward.x()) * halfX +
        std::fabs(forward.y()) * halfY +
        std::fabs(forward.z()) * halfZ;

    const float reqX = (halfWidthCam / qMax(0.001f, tanHalfX * safeFill)) + halfDepthCam;
    const float reqY = (halfHeightCam / qMax(0.001f, tanHalfY * safeFill)) + halfDepthCam;

    // Use "cover" policy: make at least one axis fill the viewport.
    // This is preferred for long-strip components where contain policy leaves the model too small.
    const float reqCover = qMin(reqX, reqY);
    // Keep a small guard margin to avoid slight overflow on different aspect ratios.
    const float coverDistance = reqCover * 1.05f;

    // Projection near plane in updateProj() is 0.1f; keep extra room to avoid clipping
    // when rotating long-strip models after auto-fit.
    const float nearPlane = 0.1f;
    const float nearSafety = halfDepthCam + nearPlane + 0.12f;
    return qMax(0.2f, qMax(coverDistance, nearSafety));
}

void RenderScene::OrbitCamera::updateMatrices()
{
    if (m_viewDirty)
        updateView();
    if (m_projDirty)
        updateProj();
}

void RenderScene::OrbitCamera::updateView()
{
    const float yawRad   = qDegreesToRadians(m_yaw);
    const float pitchRad = qDegreesToRadians(m_pitch);

    QVector3D forward(
        std::sin(yawRad) * std::cos(pitchRad),
        std::sin(pitchRad),
        std::cos(yawRad) * std::cos(pitchRad)
    );

    forward.normalize();

    const QVector3D worldUp(0.0f, 1.0f, 0.0f);
    QVector3D right = QVector3D::crossProduct(forward, worldUp).normalized();
    QVector3D up = QVector3D::crossProduct(right, forward).normalized();

    const QVector3D eye = m_target - forward * m_distance;

    m_view.setToIdentity();
    m_view.lookAt(eye, m_target, up);

    m_viewDirty = false;
}

void RenderScene::OrbitCamera::updateProj()
{
    const float aspect = m_viewport.height() > 0
        ? float(m_viewport.width()) / float(m_viewport.height())
        : 1.0f;

    m_proj.setToIdentity();
    m_proj.perspective(m_fovY, aspect, 0.1f, 1000.0f);

    m_projDirty = false;
}

// ---------------- RenderScene ----------------

RenderScene::RenderScene()
    : m_renderer()
    , m_camera()
    , m_inited(false)
    , m_cameraConfigLoaded(false)
{
    m_camera.setOnConfigChanged([this]() {
        bool enabled = false;
        bool ret = cfg::Config::get<bool>("usersettings/window_style/ng3d/lock_view_scale", enabled);
        if (ret && enabled) {
            nlohmann::json config = m_camera.exportConfig();
            saveCameraConfig(config);
        }
    });
}

void RenderScene::resize(uint32_t w, uint32_t h)
{
    m_camera.resize(w, h);

    if (!bgfx::isValid(m_frameBuffer))
    {
        return;
    }

    m_renderer.setRenderTarget(m_viewId, m_frameBuffer);

    if (!m_inited)
    {
        m_renderer.init(w, h);
        m_inited = true;

        if (!m_cameraConfigLoaded) {
            const nlohmann::json cameraConfig = getCameraConfig();
            if (cameraConfig.is_object()) {
                m_camera.loadConfig(cameraConfig);
                m_cameraConfigLoaded = true;
                LOG_I("[RenderScene] Camera config applied from usersettings/window_style/ng3d/camera");
            } else {
                LOG_W("[RenderScene] Camera config missing/invalid, keep retrying on next resize");
            }
        }

        if (!m_pendingHeightfieldPath.isEmpty())
        {
            const QString local = normalizeLocalPath(m_pendingHeightfieldPath);
            if (!local.isEmpty())
            {
                LOG_I("[RenderScene] Loading pending heightfield: {}", local.toStdString());
                m_renderer.loadHeightfieldFromFile(local.toUtf8().constData());
            }
        }
        if (!m_pendingDiffusePath.isEmpty())
        {
            const QString local = normalizeLocalPath(m_pendingDiffusePath);
            if (!local.isEmpty())
            {
                LOG_I("[RenderScene] Loading pending diffuse: {}", local.toStdString());
                m_renderer.loadDiffuseFromFile(local.toUtf8().constData());
            }
        }
    }
    else
    {
        m_renderer.resize(w, h);
    }
}


void RenderScene::update(float dt)
{
    if (!m_inited)
        return;

    if (!m_cameraConfigLoaded) {
        const nlohmann::json cameraConfig = getCameraConfig();
        if (cameraConfig.is_object()) {
            m_camera.loadConfig(cameraConfig);
            m_cameraConfigLoaded = true;
            LOG_I("[RenderScene] Camera config applied during update");
        }
    }

    if (!bgfx::isValid(m_frameBuffer))
    {
        return;
    }

    if (!RenderDevice::instance().isInitialized())
    {
        return;
    }
    Q_UNUSED(dt);
    m_camera.updateMatrices();
    m_renderer.update(dt, m_camera.viewData(), m_camera.projData());
    applyAutoFitIfNeeded();
}

void RenderScene::setRenderTarget(bgfx::ViewId viewId, bgfx::FrameBufferHandle framebuffer)
{
    m_viewId = viewId;
    m_frameBuffer = framebuffer;
    m_renderer.setRenderTarget(viewId, framebuffer);
}

void RenderScene::setOverlayRects(const std::vector<OverlayRect>& rects)
{
    m_renderer.setOverlayRects(rects);
}

void RenderScene::clearOverlayRects()
{
    m_renderer.clearOverlayRects();
}

void RenderScene::setOverlayUseScreenSpace(bool enabled)
{
    m_renderer.setOverlayUseScreenSpace(enabled);
}

void RenderScene::setOverlayDebugAxes(bool enabled)
{
    m_renderer.setOverlayDebugAxes(enabled);
}

void RenderScene::setOverlayPixelScale(float scale)
{
    m_renderer.setOverlayPixelScale(scale);
}

void RenderScene::setImageTransform(float rotationDeg, float scaleX, float scaleY)
{
    m_renderer.setImageTransform(rotationDeg, scaleX, scaleY);
    // Scale updates can change visible model size significantly; retry auto-fit once.
    m_autoFitPending = true;
}

void RenderScene::setHeightPixelSize(float pixelSize)
{
    m_renderer.setHeightPixelSize(pixelSize);
    m_autoFitPending = true;
}

void RenderScene::requestOverlayMaxReadback()
{
    m_renderer.requestOverlayMaxReadback();
}

bool RenderScene::processOverlayMaxReadback(uint32_t frameId)
{
    return m_renderer.processOverlayMaxReadback(frameId);
}

bool RenderScene::overlayMaxReady() const
{
    return m_renderer.overlayMaxReady();
}

bool RenderScene::hasOverlayRects() const
{
    return m_renderer.hasOverlayRects();
}

bool RenderScene::pickOverlayRect(const QPointF& pos, int& outId) const
{
    outId = m_renderer.pickOverlayRect(float(pos.x()), float(pos.y()));
    return outId != -1;
}

bool RenderScene::focusOverlayRect(int rectId)
{
    float targetYaw = 0.0f;
    int resolvedRectId = rectId;
    bool allowOppositeFace = true;
    if (rectId == -1)
    {
        if (!m_renderer.getAlgorithmDenseSideTargetYaw(targetYaw, resolvedRectId))
        {
            LOG_D("[RenderScene] focusOverlayRect: no valid dense algorithm side target");
            return false;
        }
        // Auto dense-side alignment must face the chosen side directly,
        // not the opposite side selected by shortest-angle heuristic.
        allowOppositeFace = false;
    }
    else if (!m_renderer.getOverlayRectNearestEdgeTargetYaw(rectId, targetYaw))
    {
        LOG_D("[RenderScene] focusOverlayRect: no valid target yaw for rect id={}", rectId);
        return false;
    }

    const nlohmann::json cameraConfig = getCameraConfig();
    if (cameraConfig.is_object())
    {
        m_camera.loadConfig(cameraConfig);
    }
    else
    {
        LOG_W("[RenderScene] focusOverlayRect: camera config missing, keep current camera");
    }

    const nlohmann::json current = m_camera.exportConfig();
    const float baseYaw = current.value("yaw", 0.0f);
    const float yawA = targetYaw;
    const float yawB = targetYaw + 180.0f;

    auto normalizeDeg = [](float a) -> float {
        while (a > 180.0f) a -= 360.0f;
        while (a < -180.0f) a += 360.0f;
        return a;
    };
    auto absDelta = [&](float to) -> float {
        return std::fabs(normalizeDeg(to - baseYaw));
    };

    const float resolvedYaw = allowOppositeFace
        ? (absDelta(yawA) <= absDelta(yawB) ? yawA : yawB)
        : yawA;

    nlohmann::json updated = current;
    updated["yaw"] = resolvedYaw;
    m_camera.loadConfig(updated);

    LOG_I("[RenderScene] focusOverlayRect: id={}, resolvedRectId={}, allowOppositeFace={}, baseYaw={:.3f}, yawA={:.3f}, yawB={:.3f}, resolvedYaw={:.3f}",
          rectId, resolvedRectId, allowOppositeFace ? 1 : 0, baseYaw, yawA, yawB, resolvedYaw);
    return true;
}

void RenderScene::loadHeightfield(const QString& path)
{
    if (path.isEmpty())
    {
        clearHeightfield();
        return;
    }

    m_pendingHeightfieldPath = path;
    m_autoFitPending = true;

    if (!m_inited)
    {
        LOG_I("[RenderScene] Heightfield pending (not inited): {}", path.toStdString());
        return;
    }

    const QString local = normalizeLocalPath(path);
    if (local.isEmpty())
    {
        LOG_W("[RenderScene] loadHeightfield: invalid path after normalization: {}", path.toStdString());
        return;
    }

    LOG_I("[RenderScene] Loading heightfield: {}", local.toStdString());
    m_renderer.loadHeightfieldFromFile(local.toUtf8().constData());
}

void RenderScene::loadDiffuse(const QString& path)
{
    if (path.isEmpty())
    {
        clearDiffuse();
        return;
    }

    m_pendingDiffusePath = path;

    if (!m_inited)
    {
        LOG_I("[RenderScene] Diffuse pending (not inited): {}", path.toStdString());
        return;
    }

    const QString local = normalizeLocalPath(path);
    if (local.isEmpty())
    {
        LOG_W("[RenderScene] loadDiffuse: invalid path after normalization: {}", path.toStdString());
        return;
    }

    LOG_I("[RenderScene] Loading diffuse: {}", local.toStdString());
    m_renderer.loadDiffuseFromFile(local.toUtf8().constData());
}

void RenderScene::clearHeightfield()
{
    m_pendingHeightfieldPath.clear();
    m_autoFitPending = false;
    m_renderer.clearHeightfield();
}

void RenderScene::clearDiffuse()
{
    m_pendingDiffusePath.clear();
    m_renderer.clearDiffuse();
}


void RenderScene::setWireframe(bool on)
{
    if (!m_inited)
        return;
    m_renderer.setWireframe(on);
}

void RenderScene::setCulling(bool on)
{
    if (!m_inited)
        return;
    m_renderer.setCulling(on);
}

void RenderScene::setFreeze(bool on)
{
    if (!m_inited)
        return;
    m_renderer.setFreeze(on);
}

void RenderScene::setGpuSubdivision(int lvl)
{
    if (!m_inited)
        return;
    m_renderer.setGpuSubdivision(lvl);
}

void RenderScene::reloadTextures()
{
    if (!m_inited)
        return;
    m_renderer.reloadTextures();
}

// Qt input forwarding.
void RenderScene::handleMousePress(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton || (e->buttons() & Qt::LeftButton))
    {
        m_camera.beginRotate(e->localPos());
        e->accept();
        return;
    }
    if (e->button() == Qt::MiddleButton || (e->buttons() & Qt::MiddleButton))
    {
        m_camera.beginPan(e->localPos());
        e->accept();
        return;
    }
    if (e->button() == Qt::RightButton || (e->buttons() & Qt::RightButton))
    {
        m_camera.beginZoomDrag(e->localPos());
        e->accept();
        return;
    }
}

void RenderScene::handleMouseMove(QMouseEvent* e)
{
    if (e->buttons() & Qt::LeftButton)
    {
        m_camera.rotateTo(e->localPos());
        e->accept();
        return;
    }
    if (e->buttons() & Qt::MiddleButton)
    {
        m_camera.panTo(e->localPos());
        e->accept();
        return;
    }
    if (e->buttons() & Qt::RightButton)
    {
        m_camera.zoomDragTo(e->localPos());
        e->accept();
        return;
    }
}

void RenderScene::handleMouseRelease(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
    {
        m_camera.endRotate();
        e->accept();
        return;
    }
    if (e->button() == Qt::MiddleButton)
    {
        m_camera.endPan();
        e->accept();
        return;
    }
    if (e->button() == Qt::RightButton)
    {
        m_camera.endZoomDrag();
        e->accept();
        return;
    }
}

void RenderScene::handleWheel(QWheelEvent* e)
{
    m_camera.zoom(float(e->angleDelta().y()));
    e->accept();
}

void RenderScene::applyAutoFitIfNeeded()
{
    if (!m_autoFitPending || !m_renderer.isHeightfieldReady())
    {
        return;
    }

    const float sx = std::fabs(m_renderer.imageScaleX());
    const float sy = std::fabs(m_renderer.imageScaleY());
    const float halfX = qMax(0.02f, m_renderer.terrainAspectRatio() * sx);
    const float halfY = qMax(0.02f, sy);
    const float halfZ = qMax(0.05f, m_renderer.dmapScale());
    const float fitDistance = m_camera.computeFitDistance(halfX, halfY, halfZ, 0.96f);
    const float currentDistance = m_camera.distance();

    // Only move closer when the model is too small in viewport.
    if (fitDistance + 0.01f < currentDistance)
    {
        m_camera.setDistance(fitDistance, false);
        LOG_I("[RenderScene] Auto-fit camera distance: current={:.3f}, fit={:.3f}, aspect={:.4f}, scale=({:.4f},{:.4f}), halfExtents=({:.3f}, {:.3f}, {:.3f})",
              currentDistance, fitDistance, m_renderer.terrainAspectRatio(), sx, sy, halfX, halfY, halfZ);
    }
    else
    {
        LOG_D("[RenderScene] Auto-fit skipped: current={:.3f}, fit={:.3f}, aspect={:.4f}, scale=({:.4f},{:.4f})",
              currentDistance, fitDistance, m_renderer.terrainAspectRatio(), sx, sy);
    }
    m_autoFitPending = false;
}
