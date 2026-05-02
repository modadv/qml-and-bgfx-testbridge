#include "screen_grabber.h"

#include <QGuiApplication>
#include <QWindow>
#include <QQuickWindow>
#include <QImage>
#include <QBuffer>
#include <QByteArray>
#include <QMetaObject>

namespace testbridge {

ScreenGrabber::ScreenGrabber(QObject* parent)
    : QObject(parent)
{}

nlohmann::json ScreenGrabber::grab(const nlohmann::json& params)
{
    QString targetName;
    if (params.contains("windowObjectName"))
        targetName = QString::fromStdString(params["windowObjectName"].get<std::string>());

    nlohmann::json result = nullptr;

    QMetaObject::invokeMethod(this, [&]() {
        QQuickWindow* target = nullptr;

        for (auto* win : QGuiApplication::allWindows()) {
            auto* qw = qobject_cast<QQuickWindow*>(win);
            if (!qw) continue;
            if (targetName.isEmpty() || qw->objectName() == targetName) {
                target = qw;
                break;
            }
        }

        if (!target) {
            if (targetName.isEmpty())
                throw std::runtime_error("No QQuickWindow found");
            else
                throw std::runtime_error("Window not found: " + targetName.toStdString());
        }

        QImage img = target->grabWindow();
        if (img.isNull())
            throw std::runtime_error("grabWindow() returned null image");

        QByteArray bytes;
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        buf.close();

        result = bytes.toBase64().toStdString();
    }, Qt::DirectConnection);

    return result;
}

} // namespace testbridge
