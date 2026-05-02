#pragma once

#include <QObject>
#include <nlohmann/json.hpp>

namespace testbridge {

class ScreenGrabber : public QObject
{
    Q_OBJECT
public:
    explicit ScreenGrabber(QObject* parent = nullptr);

    // Grabs a QQuickWindow and returns a base64-encoded PNG string.
    // `params` may contain optional `"windowObjectName"` to select a specific window.
    nlohmann::json grab(const nlohmann::json& params);
};

} // namespace testbridge
