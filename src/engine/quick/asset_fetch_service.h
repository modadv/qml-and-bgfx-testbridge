// asset_fetch_service.h
#pragma once
#include "asset_utils.h"

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QFileInfo>

#include <memory>

class AssetFetchService : public QObject {
    Q_OBJECT
public:
    explicit AssetFetchService(QObject* parent=nullptr);

    // Start an asset fetch and return the stable sha1(url) cache key.
    Q_INVOKABLE QString request(const QUrl& url);

    // Return a stable local asset path when already cached.
    Q_INVOKABLE QString cachedPath(const QUrl& url) const;

    Q_INVOKABLE QString assetsRoot() const { return m_assetsRoot; }
    Q_INVOKABLE QUrl pathToUrl(const QString& path) const {
        return QUrl::fromLocalFile(path);
    }
signals:
    void ready(QString key, QString filePath);
    void failed(QString key, int code, QString msg);

private:
    QString keyFor(const QUrl& url) const { return sha1Hex(url.toString()); }
    static QString guessExtFromContentType(const QString& ct);
    static QString guessExtFromUrl(const QUrl& url);

    QString finalPathFor(const QString& key, const QString& ext) const {
        return m_assetsRoot + "/data/" + key + (ext.isEmpty() ? "" : "." + ext);
    }

private:
    std::unique_ptr<QNetworkAccessManager> _nam;
    std::unique_ptr<QNetworkDiskCache> _disk; // Bandwidth cache; stable paths are managed separately.
    QString m_assetsRoot;
};
