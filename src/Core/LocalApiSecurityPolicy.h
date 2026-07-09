/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_LocalApiSecurityPolicy_h
#define GC_LocalApiSecurityPolicy_h

#include <QByteArray>
#include <QMultiMap>
#include <QSettings>
#include <QString>
#include <QtGlobal>

namespace LocalApiSecurityPolicy {

enum class Decision {
    Allow,
    RejectHost,
    RejectOrigin,
    RejectAuthorization
};

struct Configuration {
    QByteArray bearerToken;
    quint16 port = 0;

    bool isValid() const;
};

QByteArray generateBearerToken();
bool ensureSettingsFile(const QString &path,
                        const QByteArray &defaultContents,
                        QString *error = nullptr);
bool isWellFormedBearerToken(const QByteArray &token);
Configuration prepareServerConfiguration(QSettings &settings,
                                         QString *error = nullptr);
Decision evaluateRequest(
    const QMultiMap<QByteArray, QByteArray> &headers,
    const QByteArray &expectedBearerToken,
    quint16 port);

} // namespace LocalApiSecurityPolicy

#endif
