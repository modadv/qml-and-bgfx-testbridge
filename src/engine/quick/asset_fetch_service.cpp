// asset_fetch_service.cpp
#include "asset_fetch_service.h"
#include <QDir>
#include <QFile>
#include <QDebug>
#include <cstring>
#include "logger.h"

namespace {
bool hasValidSignature(const QString& path)
{
    QFileInfo info(path);
    const QString ext = info.suffix().toLower();
    if (ext.isEmpty()) {
        return true;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QByteArray head = file.read(12);
    if (head.size() < 2) {
        return false;
    }

    if (ext == "png") {
        static const unsigned char sig[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
        return head.size() >= 8 && std::memcmp(head.constData(), sig, sizeof(sig)) == 0;
    }
    if (ext == "jpg" || ext == "jpeg") {
        return static_cast<unsigned char>(head[0]) == 0xFF
            && static_cast<unsigned char>(head[1]) == 0xD8;
    }
    if (ext == "bmp") {
        return head.size() >= 2 && head.startsWith("BM");
    }
    if (ext == "tif" || ext == "tiff") {
        return head.size() >= 4
            && ((head[0] == 'I' && head[1] == 'I' && head[2] == '*' && head[3] == '\0')
                || (head[0] == 'M' && head[1] == 'M' && head[2] == '\0' && head[3] == '*'));
    }
    if (ext == "webp") {
        return head.size() >= 12
            && head.mid(0, 4) == "RIFF"
            && head.mid(8, 4) == "WEBP";
    }
    if (ext == "r16") {
        const qint64 size = file.size();
        return size > 0 && (size % 2) == 0;
    }

    return true;
}

bool isHttpStatusSuccess(const QVariant& status)
{
    if (!status.isValid()) {
        return true;
    }
    const int code = status.toInt();
    return code >= 200 && code < 300;
}
} // namespace

AssetFetchService::AssetFetchService(QObject* parent)
    : QObject(parent)
{
    // Stable asset root: <AppLocalData>/assets/data.
    m_assetsRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/assets";
    QDir().mkpath(m_assetsRoot + "/data");

    // QNetworkDiskCache reduces bandwidth; stable paths still use m_assetsRoot.
    _nam  = std::make_unique<QNetworkAccessManager>(this);
    _disk = std::make_unique<QNetworkDiskCache>(_nam.get());
    _disk->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/netcache");
    _disk->setMaximumCacheSize(512ll * 1024 * 1024);
    _nam->setCache(_disk.get());
}

QString AssetFetchService::cachedPath(const QUrl& url) const {
    const QString k = keyFor(url);
    const QString base = m_assetsRoot + "/data/" + k;
    const QStringList exts = QStringList() << "" << ".r16" << ".png" << ".jpg" << ".jpeg" << ".tif" << ".bmp" << ".webp";
    for (int i = 0; i < exts.size(); ++i) {
        const QString p = base + exts.at(i);
        if (QFileInfo::exists(p)) {
            if (hasValidSignature(p)) {
                return p;
            }
            LOG_W("[AssetFetchService] cache invalid signature, removing: {}", p.toStdString());
            QFile::remove(p);
        }
    }
    return QString();
}


QString AssetFetchService::guessExtFromContentType(const QString& ct) {
    const auto s = ct.toLower();
    if (s.contains("x-heightfield-r16")) return "r16";
    if (s.contains("png"))  return "png";
    if (s.contains("jpeg") || s.contains("jpg")) return "jpg";
    if (s.contains("tiff")) return "tif";
    if (s.contains("bmp"))  return "bmp";
    if (s.contains("webp")) return "webp";
    return {};
}

QString AssetFetchService::guessExtFromUrl(const QUrl& url) {
    const auto path = url.path().toLower();
    if (path.endsWith(".r16"))  return "r16";
    if (path.endsWith(".png"))  return "png";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "jpg";
    if (path.endsWith(".tif") || path.endsWith(".tiff")) return "tif";
    if (path.endsWith(".bmp"))  return "bmp";
    if (path.endsWith(".webp")) return "webp";
    return {};
}

QString AssetFetchService::request(const QUrl& url) {
    const QString key = keyFor(url);

    // Serve stable asset-cache hits immediately.
    const QString hit = cachedPath(url);
    if (!hit.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, key, hit]() {
            emit ready(key, hit);
            }, Qt::QueuedConnection);
        return key;
    }

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);

    QNetworkReply* reply = _nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, key, url]() {
        reply->deleteLater();

        const QVariant statusAttr = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int statusCode = statusAttr.isValid() ? statusAttr.toInt() : -1;
        if (reply->error() != QNetworkReply::NoError
            || !isHttpStatusSuccess(statusAttr)) {
            LOG_W("[AssetFetchService] request failed url={} status={} error={} msg={}",
                  url.toString().toStdString(), statusCode,
                  int(reply->error()), reply->errorString().toStdString());
            emit failed(key, reply->error(), reply->errorString());
            return;
        }

        const QByteArray body = reply->readAll();
        QString ext = guessExtFromContentType(
            reply->header(QNetworkRequest::ContentTypeHeader).toString());
        if (ext.isEmpty()) ext = guessExtFromUrl(url);

        const QString finalPath = finalPathFor(key, ext);
        const QString tmp = finalPath + ".tmp";

        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly) || f.write(body) != body.size()) {
            f.close();
            QFile::remove(tmp);
            emit failed(key, -1, QStringLiteral("write temp file failed"));
            return;
        }
        f.flush();
        f.close();

        QFile::remove(finalPath);
        if (!QFile::rename(tmp, finalPath)) {
            QFile::remove(tmp);
            emit failed(key, -1, QStringLiteral("atomic rename failed"));
            return;
        }

        if (!hasValidSignature(finalPath)) {
            LOG_W("[AssetFetchService] invalid response signature url={} path={}",
                  url.toString().toStdString(), finalPath.toStdString());
            QFile::remove(finalPath);
            emit failed(key, -1, QStringLiteral("invalid response signature"));
            return;
        }

        emit ready(key, finalPath);
        });

    return key;
}
