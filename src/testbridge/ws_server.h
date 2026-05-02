#pragma once

#include <QObject>
#include <QtGlobal>

#include <functional>
#include <string>

class QWebSocketServer;
class QWebSocket;
class QTimer;

namespace testbridge {

// WebSocket server wrapping QWebSocketServer.
// Runs on the main Qt thread (piggybacks off QCoreApplication event loop).
// Single concurrent WS client enforced.
// Idle timeout: if no incoming frame within kIdleTimeoutMs, close the client.
class WsServer : public QObject
{
    Q_OBJECT

public:
    static constexpr int kIdleTimeoutMs = 30 * 1000;

    // Called when a text frame arrives.
    using MessageCallback = std::function<void(const std::string& msg)>;

    explicit WsServer(QObject* parent = nullptr);
    ~WsServer() override;

    // `onMessage` receives incoming frames.
    bool start(quint16 port, MessageCallback onMessage);
    void stop();

    // Send a text message to the connected client (call from main thread).
    void send(const std::string& msg);

private slots:
    void onNewConnection();
    void onTextMessageReceived(const QString& message);
    void onClientDisconnected();
    void onIdleTimeout();

private:
    QWebSocketServer* m_server{nullptr};
    QWebSocket*       m_conn{nullptr};
    QTimer*           m_idleTimer{nullptr};
    MessageCallback   m_onMessage;
};

} // namespace testbridge
