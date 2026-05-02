#pragma once

#include <QObject>
#include <QPointer>
#include <QMutex>
#include <QHash>
#include <nlohmann/json.hpp>

namespace testbridge {

// QML reflection probe.
// All methods that touch QObject/QQuickItem MUST run on the GUI thread.
// These methods are called from the IO thread and dispatch internally.
class QmlProbe : public QObject
{
    Q_OBJECT
public:
    explicit QmlProbe(QObject* parent = nullptr);

    nlohmann::json list(const nlohmann::json& params);
    nlohmann::json find(const nlohmann::json& params);
    nlohmann::json get(const nlohmann::json& params);
    nlohmann::json set(const nlohmann::json& params);
    nlohmann::json invoke(const nlohmann::json& params);
    nlohmann::json click(const nlohmann::json& params);
    nlohmann::json type(const nlohmann::json& params);
    nlohmann::json geometry(const nlohmann::json& params);
    nlohmann::json meta(const nlohmann::json& params);
    nlohmann::json mouse(const nlohmann::json& params);
    nlohmann::json key(const nlohmann::json& params);
    nlohmann::json tree(const nlohmann::json& params);
    nlohmann::json hit(const nlohmann::json& params);

private:
    // Handle registry: uint64 handle -> QPointer<QObject>
    QHash<quint64, QPointer<QObject>> m_registry;
    QMutex                            m_regMutex;

    quint64 registerObject(QObject* obj);
    QObject* resolveHandle(quint64 handle);
};

} // namespace testbridge
