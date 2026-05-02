#pragma once

#include <QObject>
#include <QString>
#include <QVariant>
#include <QMutex>
#include <QHash>
#include <functional>
#include <nlohmann/json.hpp>

namespace testbridge {

// Adapter between GuiDataBus and JSON-RPC.
// Lives on the GUI thread; instances of this class serve as the "sender object"
// for SenderToken so events we publish don't echo back to us.
class BusEndpoint : public QObject
{
    Q_OBJECT
public:
    explicit BusEndpoint(QObject* parent = nullptr);
    ~BusEndpoint();

    // JSON-RPC method handlers (called from IO thread, dispatch to GUI thread internally)
    nlohmann::json publish(const nlohmann::json& params);
    nlohmann::json subscribe(const nlohmann::json& params);
    nlohmann::json unsubscribe(const nlohmann::json& params);
    nlohmann::json wait(const nlohmann::json& params);

    // Set a callback to stream subscribed events back to the WS client.
    using EventCallback = std::function<void(const nlohmann::json& notification)>;
    void setEventCallback(EventCallback cb);

public slots:
    void onBusEvent(const QString& topic, const QVariant& payload);

private:
    EventCallback   m_eventCb;
    QMutex          m_cbMutex;

    // Tracks which topics we have subscribed to.
    QHash<QString, bool> m_subscriptions;
    QMutex               m_subMutex;
};

} // namespace testbridge
