#pragma once

#include <QObject>
#include <QMutex>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace testbridge {

class WsServer;
class RpcDispatcher;
class BusEndpoint;
class QmlProbe;
class ScreenGrabber;

// TestBridge facade.
// Call start() after the Qt app is running (after engine.load).
// Call stop() in QCoreApplication::aboutToQuit.
class TestBridge : public QObject
{
    Q_OBJECT
public:
    static TestBridge& instance();

    // Start the WebSocket test bridge on `port` (default 47600).
    // `parent` is accepted for host API compatibility; the singleton owns its
    // components and must be stopped from QCoreApplication::aboutToQuit.
    bool start(QObject* parent, quint16 port = 47600);
    void stop();

    using JsonProvider = std::function<nlohmann::json()>;
    using JsonHandler = std::function<nlohmann::json(const nlohmann::json&)>;
    void setRenderCapsProvider(JsonProvider provider);
    void setRenderStatsProvider(JsonProvider provider);
    void setRenderResourcesProvider(JsonProvider provider);
    void setShaderCompileHandler(JsonHandler handler);
    void setShaderApplyHandler(JsonHandler handler);
    void setShaderRevertHandler(JsonHandler handler);
    void setShaderListProvider(JsonProvider provider);

private:
    explicit TestBridge(QObject* parent = nullptr);
    ~TestBridge();

    void registerMethods();
    nlohmann::json callProvider(const JsonProvider& provider,
                                const char* name) const;
    nlohmann::json callHandler(const JsonHandler& handler,
                               const nlohmann::json& params,
                               const char* name) const;

    std::unique_ptr<WsServer>      m_server;
    std::unique_ptr<RpcDispatcher> m_dispatcher;
    std::unique_ptr<BusEndpoint>   m_bus;
    std::unique_ptr<QmlProbe>      m_qml;
    std::unique_ptr<ScreenGrabber> m_screen;

    mutable QMutex m_providerMutex;
    JsonProvider m_renderCapsProvider;
    JsonProvider m_renderStatsProvider;
    JsonProvider m_renderResourcesProvider;
    JsonHandler m_shaderCompileHandler;
    JsonHandler m_shaderApplyHandler;
    JsonHandler m_shaderRevertHandler;
    JsonProvider m_shaderListProvider;

    bool m_started{false};
};

} // namespace testbridge
