// render_scene.h
#pragma once

#include "terrain/terrain_renderer.h"
#include "config/config.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <vector>

#include <QMatrix4x4>
#include <QPointF>
#include <QSize>
#include <QVector3D>
#include <QString>
#include <QWheelEvent>

class RenderScene
{
public:
    RenderScene();
    ~RenderScene() = default;

    void resize(uint32_t w, uint32_t h);
    void update(float dt);
    void loadHeightfield(const QString& path);
    void loadDiffuse(const QString& path);
    void clearHeightfield();
    void clearDiffuse();
    void setWireframe(bool on);
    void setCulling(bool on);
    void setFreeze(bool on);
    void setGpuSubdivision(int lvl);
    void reloadTextures();
    void setRenderTarget(bgfx::ViewId viewId, bgfx::FrameBufferHandle framebuffer);
    void setOverlayRects(const std::vector<OverlayRect>& rects);
    void clearOverlayRects();
    void setOverlayUseScreenSpace(bool enabled);
    void setOverlayDebugAxes(bool enabled);
    void setOverlayPixelScale(float scale);
    void setImageTransform(float rotationDeg, float scaleX, float scaleY);
    void setHeightPixelSize(float pixelSize);
    void requestLiveTerrainSimpleFragment(const std::string& binPath,
                                          const std::string& hash)
    {
        m_renderer.requestLiveTerrainSimpleFragment(binPath, hash);
    }
    void requestRevertLiveTerrainSimpleFragment()
    {
        m_renderer.requestRevertLiveTerrainSimpleFragment();
    }
    void requestOverlayMaxReadback();
    bool processOverlayMaxReadback(uint32_t frameId);
    bool overlayMaxReady() const;
    bool hasOverlayRects() const;
    nlohmann::json performanceSnapshot() const { return m_renderer.performanceSnapshot(); }
    nlohmann::json resourcesSnapshot() const
    {
        nlohmann::json out = m_renderer.resourcesSnapshot();
        out["sceneInitialized"] = m_inited;
        out["viewId"] = m_viewId;
        out["framebufferValid"] = bgfx::isValid(m_frameBuffer);
        out["camera"] = m_camera.exportConfig();
        return out;
    }
    bool pickOverlayRect(const QPointF& pos, int& outId) const;
    bool focusOverlayRect(int rectId);

    void handleMousePress(QMouseEvent* e);
    void handleMouseMove(QMouseEvent* e);
    void handleMouseRelease(QMouseEvent* e);
    void handleWheel(QWheelEvent* e);

    void saveCameraConfig(const nlohmann::json& config) {
        cfg::Config::set("usersettings/window_style/ng3d/camera", config);
        cfg::Config::save();
    }

    nlohmann::json getCameraConfig()
    {
        nlohmann::json config;
        if (!cfg::Config::getJson("usersettings/window_style/ng3d/camera", config)) {
            nlohmann::json defaultConfig;
            if (cfg::Config::getDefaultJson("usersettings/window_style/ng3d/camera", defaultConfig)
                && defaultConfig.is_object()) {
                // Backfill missing user config from embedded defaults.
                cfg::Config::set("usersettings/window_style/ng3d/camera", defaultConfig);
                cfg::Config::save();
                return defaultConfig;
            }

            // Fallback for environments where default resource parsing fails.
            nlohmann::json hardcoded = {
                {"distance", 2.5},
                {"fovY", 60.0},
                {"pitch", 30.6},
                {"target", {
                    {"x", 0.02},
                    {"y", -0.122},
                    {"z", -0.073}
                }},
                {"yaw", 181.136}
            };
            cfg::Config::set("usersettings/window_style/ng3d/camera", hardcoded);
            cfg::Config::save();
            return hardcoded;
        }
        return config;
    }
private:
    struct OrbitCamera
    {
        void resize(uint32_t w, uint32_t h);
        void beginRotate(const QPointF& pos);
        void rotateTo(const QPointF& pos);
        void endRotate();
        void beginPan(const QPointF& pos);
        void panTo(const QPointF& pos);
        void endPan();
        void beginZoomDrag(const QPointF& pos);
        void zoomDragTo(const QPointF& pos);
        void endZoomDrag();
        void zoom(float delta);
        void updateMatrices();
        float distance() const { return m_distance; }
        void setDistance(float value, bool notify = true);
        float computeFitDistance(float halfX, float halfY, float halfZ, float fillRatio) const;

        const float* viewData() const { return m_view.constData(); }
        const float* projData() const { return m_proj.constData(); }

        nlohmann::json exportConfig() const;
        void loadConfig(const nlohmann::json& config);
        void setOnConfigChanged(std::function<void()> callback) { m_onConfigChanged = callback; }

    private:
        void updateView();
        void updateProj();
        void notifyConfigChanged();
        void logState(const char* reason) const;

        QPointF   m_lastPos;
        QPointF   m_lastPanPos;
        QPointF   m_lastZoomPos;
        QSize     m_viewport            = QSize(1, 1);
        QVector3D m_target              = QVector3D(0.0f, 0.0f, 0.0f);
        float     m_yaw                 = 0.0f;
        float     m_pitch               = 0.0f;
        float     m_distance            = 3.0f;
        float     m_fovY                = 60.0f;
        bool      m_rotating            = false;
        bool      m_panning             = false;
        bool      m_zooming             = false;
        bool      m_viewDirty           = true;
        bool      m_projDirty           = true;
        QMatrix4x4 m_view;
        QMatrix4x4 m_proj;
        
        std::function<void()> m_onConfigChanged;
    };

    TerrainRenderer m_renderer;
    OrbitCamera       m_camera;
    bool              m_inited = false;
    bgfx::ViewId            m_viewId = 0;
    bgfx::FrameBufferHandle m_frameBuffer = BGFX_INVALID_HANDLE;
    QString           m_pendingHeightfieldPath;
    QString           m_pendingDiffusePath;
    bool              m_autoFitPending = false;
    
    bool m_cameraConfigLoaded = false;

    void applyAutoFitIfNeeded();
};
