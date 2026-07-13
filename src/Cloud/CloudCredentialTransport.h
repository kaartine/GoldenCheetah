/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_CloudCredentialTransport_h
#define GC_CloudCredentialTransport_h

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

namespace CloudCredentialTransport {

struct Request {
    QNetworkRequest request;
    QByteArray body;
};

Request makeWithingsMeasuresRequest(const QUrl &endpoint,
                                    const QString &accessToken,
                                    const QDateTime &from,
                                    const QDateTime &to);
Request makeRideWithGpsAuthTokenRequest(const QUrl &endpoint,
                                        const QString &apiKey,
                                        const QString &email,
                                        const QString &password);
QString rideWithGpsAuthToken(const QJsonObject &response);

} // namespace CloudCredentialTransport

#endif
