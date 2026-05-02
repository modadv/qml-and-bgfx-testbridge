#include "bus_endpoint.h"

#include "quick/guidatabus.h"

#include <QMetaObject>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QJsonValue>

// WaitHelper must be at file scope so moc can process Q_OBJECT.
class BusWaitHelper : public QObject {
    Q_OBJECT
public:
    bool*        fired  = nullptr;
    QVariant*    result = nullptr;
    QEventLoop*  loop   = nullptr;
public slots:
    void onBusEvent(const QString& /*t*/, const QVariant& payload) {
        if (fired)  *fired  = true;
        if (result) *result = payload;
        if (loop)   loop->quit();
    }
};

namespace testbridge {

// Helper: convert QVariant to nlohmann::json (best-effort)
static nlohmann::json variantToJson(const QVariant& v)
{
    if (!v.isValid() || v.isNull())
        return nullptr;
    switch (static_cast<int>(v.type())) {
        case QMetaType::Bool:       return v.toBool();
        case QMetaType::Int:
        case QMetaType::Long:
        case QMetaType::LongLong:   return v.toLongLong();
        case QMetaType::UInt:
        case QMetaType::ULong:
        case QMetaType::ULongLong:  return v.toULongLong();
        case QMetaType::Double:
        case QMetaType::Float:      return v.toDouble();
        case QMetaType::QString:    return v.toString().toStdString();
        default:
            return v.toString().toStdString();
    }
}

// Helper: convert nlohmann::json to QVariant
static QVariant jsonToVariant(const nlohmann::json& j)
{
    if (j.is_null())    return QVariant();
    if (j.is_boolean()) return QVariant(j.get<bool>());
    if (j.is_number_integer()) return QVariant(static_cast<qlonglong>(j.get<long long>()));
    if (j.is_number_float())   return QVariant(j.get<double>());
    if (j.is_string())  return QVariant(QString::fromStdString(j.get<std::string>()));
    // Arrays / objects: convert to string representation
    return QVariant(QString::fromStdString(j.dump()));
}

BusEndpoint::BusEndpoint(QObject* parent)
    : QObject(parent)
{}

BusEndpoint::~BusEndpoint()
{
    // Unregister all subscriptions
    QMutexLocker lk(&m_subMutex);
    for (auto it = m_subscriptions.begin(); it != m_subscriptions.end(); ++it) {
        GuiDataBus::instance()->unregisterHandler(it.key(), this);
    }
}

void BusEndpoint::setEventCallback(EventCallback cb)
{
    QMutexLocker lk(&m_cbMutex);
    m_eventCb = std::move(cb);
}

nlohmann::json BusEndpoint::publish(const nlohmann::json& params)
{
    if (!params.contains("topic"))
        throw std::invalid_argument("params.topic required");

    QString topic   = QString::fromStdString(params["topic"].get<std::string>());
    QVariant payload;
    if (params.contains("payload"))
        payload = jsonToVariant(params["payload"]);

    // Post from GUI thread
    auto token = GuiDataBus::SenderToken::fromObject(this);
    QMetaObject::invokeMethod(this, [topic, payload, token]() {
        GuiDataBus::instance()->post(token, topic, payload);
    }, Qt::QueuedConnection);

    return true;
}

nlohmann::json BusEndpoint::subscribe(const nlohmann::json& params)
{
    if (!params.contains("topic"))
        throw std::invalid_argument("params.topic required");
    QString topic = QString::fromStdString(params["topic"].get<std::string>());

    {
        QMutexLocker lk(&m_subMutex);
        if (m_subscriptions.contains(topic))
            return true; // already subscribed
        m_subscriptions[topic] = true;
    }

    // Register handler on GUI thread
    QMetaObject::invokeMethod(this, [this, topic]() {
        GuiDataBus::instance()->registerHandler(topic, this, "onBusEvent");
    }, Qt::QueuedConnection);

    return true;
}

nlohmann::json BusEndpoint::unsubscribe(const nlohmann::json& params)
{
    if (!params.contains("topic"))
        throw std::invalid_argument("params.topic required");
    QString topic = QString::fromStdString(params["topic"].get<std::string>());

    {
        QMutexLocker lk(&m_subMutex);
        if (!m_subscriptions.remove(topic))
            return false;
    }

    QMetaObject::invokeMethod(this, [this, topic]() {
        GuiDataBus::instance()->unregisterHandler(topic, this);
    }, Qt::QueuedConnection);

    return true;
}

nlohmann::json BusEndpoint::wait(const nlohmann::json& params)
{
    if (!params.contains("topic"))
        throw std::invalid_argument("params.topic required");

    QString topic      = QString::fromStdString(params["topic"].get<std::string>());
    int timeout_ms     = params.value("timeout_ms", 5000);

    // We're on the IO thread. Use a QEventLoop on the calling thread to wait.
    // A temporary QObject registers for the event; when it fires we quit the loop.
    bool fired = false;
    QVariant result;

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(timeout_ms);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    // Create helper on GUI thread
    BusWaitHelper* helper = nullptr;
    QMetaObject::invokeMethod(GuiDataBus::instance(), [&]() {
        helper = new BusWaitHelper();
        helper->fired  = &fired;
        helper->result = &result;
        helper->loop   = &loop;
        GuiDataBus::instance()->registerHandler(topic, helper, "onBusEvent");
    }, Qt::DirectConnection);

    timer.start();
    loop.exec();

    // Cleanup on GUI thread
    QMetaObject::invokeMethod(GuiDataBus::instance(), [&]() {
        GuiDataBus::instance()->unregisterHandler(topic, helper);
        delete helper;
    }, Qt::DirectConnection);

    if (!fired) return nullptr;
    return variantToJson(result);
}

void BusEndpoint::onBusEvent(const QString& topic, const QVariant& payload)
{
    nlohmann::json notification;
    notification["jsonrpc"] = "2.0";
    notification["method"]  = "bus.event";
    notification["params"]["topic"]   = topic.toStdString();
    notification["params"]["payload"] = variantToJson(payload);

    QMutexLocker lk(&m_cbMutex);
    if (m_eventCb) m_eventCb(notification);
}

} // namespace testbridge

#include "bus_endpoint.moc"
