#pragma once

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QVariant>
#include <QVector>

class GuiDataBus : public QObject
{
    Q_OBJECT
public:
    struct SenderToken
    {
        QPointer<QObject> sender;

        static SenderToken fromObject(QObject* object)
        {
            SenderToken token;
            token.sender = object;
            return token;
        }
    };

    static GuiDataBus* instance();

    void post(const SenderToken& token, const QString& topic, const QVariant& payload);
    void registerHandler(const QString& topic, QObject* receiver, const char* slot);
    void unregisterHandler(const QString& topic, QObject* receiver);

private:
    explicit GuiDataBus(QObject* parent = nullptr);

    struct Handler
    {
        QPointer<QObject> receiver;
        QByteArray slot;
    };

    QHash<QString, QVector<Handler>> m_handlers;
};
