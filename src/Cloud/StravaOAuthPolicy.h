/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_StravaOAuthPolicy_h
#define GC_StravaOAuthPolicy_h

#include <QByteArray>
#include <QNetworkReply>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace StravaOAuthPolicy {

struct TokenRequest {
    QUrl endpoint;
    QByteArray body;
    QString error;

    bool isValid() const
    {
        return error.isEmpty()
            && endpoint.isValid()
            && !endpoint.isEmpty()
            && !body.isEmpty();
    }
};

struct TokenResponse {
    QString accessToken;
    QString refreshToken;
    QString error;

    bool isValid() const
    {
        return error.isEmpty()
            && !accessToken.isEmpty()
            && !refreshToken.isEmpty();
    }
};

bool hasUsableCredentials(const QString &clientId,
                          const QString &clientSecret);
TokenRequest authorizationCodeRequest(
    const QString &clientId,
    const QString &clientSecret,
    const QString &authorizationCode);
TokenRequest refreshTokenRequest(
    const QString &clientId,
    const QString &clientSecret,
    const QString &refreshToken);
QString tokenFailureMessage(
    int httpStatus,
    QNetworkReply::NetworkError networkError,
    const QString &networkErrorString,
    const QByteArray &payload,
    const QStringList &sensitiveValues);
TokenResponse parseTokenResponse(const QByteArray &payload);

} // namespace StravaOAuthPolicy

#endif
