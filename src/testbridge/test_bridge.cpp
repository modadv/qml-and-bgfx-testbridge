#include "test_bridge.h"

#include "ws_server.h"
#include "rpc_dispatcher.h"
#include "bus_endpoint.h"
#include "qml_probe.h"
#include "screen_grabber.h"
#include "log_sink.h"

#include <spdlog/spdlog.h>
#include "logger.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

// version_config.h may not be visible from testbridge; guard it.
#ifdef TEST_BRIDGE_ENABLED
#  ifdef APP_VERSION_FULL
#    define TESTBRIDGE_APP_VERSION APP_VERSION_FULL
#  else
#    define TESTBRIDGE_APP_VERSION "unknown"
#  endif
#endif

namespace testbridge {

TestBridge& TestBridge::instance()
{
    static TestBridge s;
    return s;
}

TestBridge::TestBridge(QObject* parent)
    : QObject(parent)
{}

TestBridge::~TestBridge()
{
    stop();
}

bool TestBridge::start(QObject* parent, quint16 port)
{
    if (m_started) return true;

    Q_UNUSED(parent)

    // Create components on the GUI thread (we're in main() here)
    m_bus      = std::unique_ptr<BusEndpoint>(new BusEndpoint());
    m_qml      = std::unique_ptr<QmlProbe>(new QmlProbe());
    m_screen   = std::unique_ptr<ScreenGrabber>(new ScreenGrabber());
    m_dispatcher = std::unique_ptr<RpcDispatcher>(new RpcDispatcher());
    m_server   = std::unique_ptr<WsServer>(new WsServer());

    registerMethods();

    // Wire server -> dispatcher -> server (send)
    BusEndpoint* busPtr   = m_bus.get();
    RpcDispatcher* dispPtr = m_dispatcher.get();
    WsServer* srvPtr      = m_server.get();

    // Stream subscribed bus events back to WS client
    busPtr->setEventCallback([srvPtr](const nlohmann::json& notif) {
        srvPtr->send(notif.dump());
    });

    if (!m_server->start(port, [dispPtr, srvPtr, this](const std::string& msg) {
        std::string resp = dispPtr->dispatch(msg, this);
        if (!resp.empty())
            srvPtr->send(resp);
    })) {
        m_server.reset();
        m_dispatcher.reset();
        m_screen.reset();
        m_qml.reset();
        m_bus.reset();
        return false;
    }

    // Attach log sink to default logger
    auto sink = LogSink::instance();
    spdlog::default_logger()->sinks().push_back(sink);
    // Also attach to project's central LOG_I/LOG_W/LOG_E/LOG_D logger
    if (auto projLogger = Logger::getInstance().getLogger()) {
        projLogger->sinks().push_back(sink);
    }

    m_started = true;
    return true;
}

void TestBridge::stop()
{
    if (!m_started) return;
    m_started = false;
    if (m_server) m_server->stop();
}

void TestBridge::setRenderCapsProvider(JsonProvider provider)
{
    QMutexLocker lock(&m_providerMutex);
    m_renderCapsProvider = std::move(provider);
}

void TestBridge::setRenderStatsProvider(JsonProvider provider)
{
    QMutexLocker lock(&m_providerMutex);
    m_renderStatsProvider = std::move(provider);
}

void TestBridge::setRenderResourcesProvider(JsonProvider provider)
{
    QMutexLocker lock(&m_providerMutex);
    m_renderResourcesProvider = std::move(provider);
}

void TestBridge::setShaderCompileHandler(JsonHandler handler)
{
    QMutexLocker lock(&m_providerMutex);
    m_shaderCompileHandler = std::move(handler);
}

void TestBridge::setShaderApplyHandler(JsonHandler handler)
{
    QMutexLocker lock(&m_providerMutex);
    m_shaderApplyHandler = std::move(handler);
}

void TestBridge::setShaderRevertHandler(JsonHandler handler)
{
    QMutexLocker lock(&m_providerMutex);
    m_shaderRevertHandler = std::move(handler);
}

void TestBridge::setShaderListProvider(JsonProvider provider)
{
    QMutexLocker lock(&m_providerMutex);
    m_shaderListProvider = std::move(provider);
}

nlohmann::json TestBridge::callProvider(const JsonProvider& provider,
                                        const char* name) const
{
    if (!provider) {
        return {
            {"available", false},
            {"provider", name},
            {"reason", "host did not register a provider"}
        };
    }
    return provider();
}

nlohmann::json TestBridge::callHandler(const JsonHandler& handler,
                                       const nlohmann::json& params,
                                       const char* name) const
{
    if (!handler) {
        return {
            {"available", false},
            {"provider", name},
            {"reason", "host did not register a handler"}
        };
    }
    return handler(params);
}

void TestBridge::registerMethods()
{
    auto* disp   = m_dispatcher.get();
    auto* bus    = m_bus.get();
    auto* qml    = m_qml.get();
    auto* screen = m_screen.get();

    // ---- app methods ----
    disp->registerMethod("app.ping", [](const nlohmann::json&) -> nlohmann::json {
        return "pong";
    });

    disp->registerMethod("app.version", [](const nlohmann::json&) -> nlohmann::json {
        return TESTBRIDGE_APP_VERSION;
    });

    disp->registerMethod("app.describe", [](const nlohmann::json&) -> nlohmann::json {
        nlohmann::json windows = nlohmann::json::array();
        for (auto* window : QGuiApplication::allWindows()) {
            nlohmann::json item;
            item["objectName"] = window->objectName().toStdString();
            item["title"] = window->title().toStdString();
            item["class"] = window->metaObject()->className();
            item["visible"] = window->isVisible();
            item["width"] = window->width();
            item["height"] = window->height();
            item["x"] = window->x();
            item["y"] = window->y();
            if (auto* screen = window->screen()) {
                item["screen"] = screen->name().toStdString();
            }
            windows.push_back(item);
        }

        return {
            {"applicationName", QCoreApplication::applicationName().toStdString()},
            {"organizationName", QCoreApplication::organizationName().toStdString()},
            {"applicationVersion", TESTBRIDGE_APP_VERSION},
            {"qtVersion", qVersion()},
            {"platformName", QGuiApplication::platformName().toStdString()},
            {"pid", QCoreApplication::applicationPid()},
            {"protocol", "testbridge-jsonrpc"},
            {"protocolVersion", "0.2.0"},
            {"methods", {
                "app.ping", "app.version", "app.describe", "app.quit",
                "bus.publish", "bus.subscribe", "bus.unsubscribe", "bus.wait",
                "qml.list", "qml.find", "qml.get", "qml.set", "qml.invoke",
                "qml.click", "qml.type", "qml.geometry", "qml.meta",
                "qml.mouse", "qml.key", "qml.tree", "qml.hit",
                "window.grab", "log.recent", "log.wait",
                "render.caps", "render.stats", "render.resources",
                "shader.compile", "shader.apply", "shader.revert", "shader.list",
                "test.artifacts"
            }},
            {"windows", windows}
        };
    });

    disp->registerMethod("app.quit", [](const nlohmann::json&) -> nlohmann::json {
        QMetaObject::invokeMethod(QCoreApplication::instance(), "quit",
                                  Qt::QueuedConnection);
        return true;
    });

    // ---- bus methods ----
    disp->registerMethod("bus.publish", [bus](const nlohmann::json& p) {
        return bus->publish(p);
    });
    disp->registerMethod("bus.subscribe", [bus](const nlohmann::json& p) {
        return bus->subscribe(p);
    });
    disp->registerMethod("bus.unsubscribe", [bus](const nlohmann::json& p) {
        return bus->unsubscribe(p);
    });
    disp->registerMethod("bus.wait", [bus](const nlohmann::json& p) {
        return bus->wait(p);
    });

    // ---- qml methods ----
    disp->registerMethod("qml.list", [qml](const nlohmann::json& p) {
        return qml->list(p);
    });
    disp->registerMethod("qml.find", [qml](const nlohmann::json& p) {
        return qml->find(p);
    });
    disp->registerMethod("qml.get", [qml](const nlohmann::json& p) {
        return qml->get(p);
    });
    disp->registerMethod("qml.set", [qml](const nlohmann::json& p) {
        return qml->set(p);
    });
    disp->registerMethod("qml.invoke", [qml](const nlohmann::json& p) {
        return qml->invoke(p);
    });
    disp->registerMethod("qml.click", [qml](const nlohmann::json& p) {
        return qml->click(p);
    });
    disp->registerMethod("qml.type", [qml](const nlohmann::json& p) {
        return qml->type(p);
    });
    disp->registerMethod("qml.geometry", [qml](const nlohmann::json& p) {
        return qml->geometry(p);
    });
    disp->registerMethod("qml.meta", [qml](const nlohmann::json& p) {
        return qml->meta(p);
    });
    disp->registerMethod("qml.mouse", [qml](const nlohmann::json& p) {
        return qml->mouse(p);
    });
    disp->registerMethod("qml.key", [qml](const nlohmann::json& p) {
        return qml->key(p);
    });
    disp->registerMethod("qml.tree", [qml](const nlohmann::json& p) {
        return qml->tree(p);
    });
    disp->registerMethod("qml.hit", [qml](const nlohmann::json& p) {
        return qml->hit(p);
    });

    // ---- window methods ----
    disp->registerMethod("window.grab", [screen](const nlohmann::json& p) {
        return screen->grab(p);
    });

    // ---- log methods ----
    auto logSink = LogSink::instance();
    disp->registerMethod("log.recent", [logSink](const nlohmann::json& p) -> nlohmann::json {
        std::size_t max = p.value("max_lines", p.value("max", std::size_t(100)));
        std::string re  = p.value("regex", std::string{});
        return logSink->recent(max, re);
    });
    disp->registerMethod("log.wait", [logSink](const nlohmann::json& p) -> nlohmann::json {
        std::string re = p.value("regex", std::string{});
        int timeout_ms = p.value("timeout_ms", 5000);
        if (re.empty()) throw std::invalid_argument("params.regex required");
        return logSink->wait(re, timeout_ms);
    });

    disp->registerMethod("render.caps", [this](const nlohmann::json&) -> nlohmann::json {
        JsonProvider provider;
        {
            QMutexLocker lock(&m_providerMutex);
            provider = m_renderCapsProvider;
        }
        return callProvider(provider, "render.caps");
    });
    disp->registerMethod("render.stats", [this](const nlohmann::json&) -> nlohmann::json {
        JsonProvider provider;
        {
            QMutexLocker lock(&m_providerMutex);
            provider = m_renderStatsProvider;
        }
        return callProvider(provider, "render.stats");
    });
    disp->registerMethod("render.resources", [this](const nlohmann::json&) -> nlohmann::json {
        JsonProvider provider;
        {
            QMutexLocker lock(&m_providerMutex);
            provider = m_renderResourcesProvider;
        }
        return callProvider(provider, "render.resources");
    });
    disp->registerMethod("test.artifacts", [](const nlohmann::json&) -> nlohmann::json {
        return {
            {"available", true},
            {"version", 1},
            {"recommendedRpc", {
                "app.describe", "qml.tree", "log.recent", "window.grab",
                "render.caps", "render.stats", "render.resources",
                "shader.list"
            }},
            {"env", {
                {"TESTBRIDGE_ARTIFACT_DIR", "optional directory used by host-side test runners"},
                {"TESTBRIDGE_PORT", "optional WebSocket port override"}
            }}
        };
    });
    disp->registerMethod("shader.compile", [this](const nlohmann::json& p) -> nlohmann::json {
        JsonHandler handler;
        {
            QMutexLocker lock(&m_providerMutex);
            handler = m_shaderCompileHandler;
        }
        return callHandler(handler, p, "shader.compile");
    });
    disp->registerMethod("shader.apply", [this](const nlohmann::json& p) -> nlohmann::json {
        JsonHandler handler;
        {
            QMutexLocker lock(&m_providerMutex);
            handler = m_shaderApplyHandler;
        }
        return callHandler(handler, p, "shader.apply");
    });
    disp->registerMethod("shader.revert", [this](const nlohmann::json& p) -> nlohmann::json {
        JsonHandler handler;
        {
            QMutexLocker lock(&m_providerMutex);
            handler = m_shaderRevertHandler;
        }
        return callHandler(handler, p, "shader.revert");
    });
    disp->registerMethod("shader.list", [this](const nlohmann::json&) -> nlohmann::json {
        JsonProvider provider;
        {
            QMutexLocker lock(&m_providerMutex);
            provider = m_shaderListProvider;
        }
        return callProvider(provider, "shader.list");
    });
}

} // namespace testbridge
