#include "ws_server.h"

#include <QWebSocketServer>
#include <QWebSocket>
#include <QByteArray>
#include <QHostAddress>
#include <QTimer>

#include <spdlog/spdlog.h>

namespace testbridge {

WsServer::WsServer(QObject* parent)
    : QObject(parent)
{}

WsServer::~WsServer()
{
    stop();
}

bool WsServer::start(quint16 port, MessageCallback onMessage)
{
    m_onMessage = std::move(onMessage);

    m_server = new QWebSocketServer(
        QStringLiteral("TestBridge"),
        QWebSocketServer::NonSecureMode,
        this);

    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        const QByteArray error = m_server->errorString().toUtf8();
        spdlog::error("WsServer: failed to listen on port {}: {}",
                      port,
                      error.constData());
        m_server->deleteLater();
        m_server = nullptr;
        return false;
    }

    connect(m_server, &QWebSocketServer::newConnection,
            this, &WsServer::onNewConnection);

    spdlog::info("WsServer: listening on 127.0.0.1:{}", port);
    return true;
}

void WsServer::stop()
{
    if (m_idleTimer) {
        m_idleTimer->stop();
        m_idleTimer->deleteLater();
        m_idleTimer = nullptr;
    }
    if (m_conn) {
        m_conn->close();
        m_conn = nullptr;
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
}

void WsServer::send(const std::string& msg)
{
    if (m_conn && m_conn->isValid()) {
        m_conn->sendTextMessage(QString::fromStdString(msg));
    }
}

void WsServer::onNewConnection()
{
    QWebSocket* pending = m_server->nextPendingConnection();
    if (!pending) return;

    if (m_conn) {
        // Enforce single-client policy
        pending->close();
        pending->deleteLater();
        return;
    }

    m_conn = pending;
    m_conn->setParent(this);

    connect(m_conn, &QWebSocket::textMessageReceived,
            this, &WsServer::onTextMessageReceived);
    connect(m_conn, &QWebSocket::disconnected,
            this, &WsServer::onClientDisconnected);

    // Start idle timeout: if the client sends no frames within kIdleTimeoutMs,
    // the server force-closes the connection. The timer is reset in
    // onTextMessageReceived() on every inbound frame.
    if (!m_idleTimer) {
        m_idleTimer = new QTimer(this);
        m_idleTimer->setSingleShot(true);
        connect(m_idleTimer, &QTimer::timeout,
                this, &WsServer::onIdleTimeout);
    }
    m_idleTimer->start(+kIdleTimeoutMs);

    spdlog::info("WsServer: client connected from {}",
                 m_conn->peerAddress().toString().toStdString());
}

void WsServer::onTextMessageReceived(const QString& message)
{
    if (m_idleTimer) {
        m_idleTimer->start(+kIdleTimeoutMs);
    }
    if (m_onMessage) {
        m_onMessage(message.toStdString());
    }
}

void WsServer::onClientDisconnected()
{
    if (m_idleTimer) {
        m_idleTimer->stop();
    }
    if (m_conn) {
        spdlog::info("WsServer: client disconnected");
        m_conn->deleteLater();
        m_conn = nullptr;
    }
}

void WsServer::onIdleTimeout()
{
    if (m_conn) {
        spdlog::info("WsServer: client idle > {}ms, closing", +kIdleTimeoutMs);
        m_conn->close(QWebSocketProtocol::CloseCodeNormal,
                      QStringLiteral("idle timeout"));
    }
}

} // namespace testbridge
