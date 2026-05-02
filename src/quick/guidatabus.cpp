#include "quick/guidatabus.h"

#include <QMetaObject>

GuiDataBus::GuiDataBus(QObject* parent)
    : QObject(parent)
{
}

GuiDataBus* GuiDataBus::instance()
{
    static GuiDataBus bus;
    return &bus;
}

void GuiDataBus::post(const SenderToken& token, const QString& topic, const QVariant& payload)
{
    auto handlers = m_handlers.value(topic);
    for (const Handler& handler : handlers)
    {
        QObject* receiver = handler.receiver.data();
        if (!receiver || receiver == token.sender.data())
            continue;

        QMetaObject::invokeMethod(receiver,
                                  handler.slot.constData(),
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, topic),
                                  Q_ARG(QVariant, payload));
    }
}

void GuiDataBus::registerHandler(const QString& topic, QObject* receiver, const char* slot)
{
    if (!receiver || !slot)
        return;

    QVector<Handler>& handlers = m_handlers[topic];
    for (const Handler& handler : handlers)
    {
        if (handler.receiver == receiver && handler.slot == slot)
            return;
    }

    handlers.push_back({receiver, QByteArray(slot)});
}

void GuiDataBus::unregisterHandler(const QString& topic, QObject* receiver)
{
    auto it = m_handlers.find(topic);
    if (it == m_handlers.end())
        return;

    QVector<Handler>& handlers = it.value();
    for (int i = handlers.size() - 1; i >= 0; --i)
    {
        if (!handlers[i].receiver || handlers[i].receiver == receiver)
            handlers.remove(i);
    }

    if (handlers.isEmpty())
        m_handlers.erase(it);
}
