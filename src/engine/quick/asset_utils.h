#pragma once
#include <QString>
#include <QCryptographicHash>
inline QString sha1Hex(const QString& s) {
    return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex());
}

