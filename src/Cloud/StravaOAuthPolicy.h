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
#include <QNetworkRequest>
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
    QStringList grantedScopes;
    QString error;

    bool isValid() const
    {
        return error.isEmpty()
            && !accessToken.isEmpty()
            && !refreshToken.isEmpty();
    }
};

enum class RevocationTokenType {
    AccessToken,
    RefreshToken
};

struct RevocationRequest {
    QUrl endpoint;
    QByteArray method;
    QByteArray authorizationHeader;
    QByteArray contentTypeHeader;
    QByteArray body;
    QNetworkRequest::RedirectPolicy redirectPolicy =
        QNetworkRequest::ManualRedirectPolicy;
    QString error;

    bool isValid() const
    {
        return error.isEmpty()
            && endpoint == QUrl(QStringLiteral(
                "https://www.strava.com/oauth/revoke"))
            && method == QByteArrayLiteral("POST")
            && authorizationHeader.startsWith(
                QByteArrayLiteral("Basic "))
            && authorizationHeader.size()
                > QByteArrayLiteral("Basic ").size()
            && contentTypeHeader == QByteArrayLiteral(
                "application/x-www-form-urlencoded")
            && !body.isEmpty()
            && redirectPolicy
                == QNetworkRequest::ManualRedirectPolicy;
    }
};

bool hasUsableCredentials(const QString &clientId,
                          const QString &clientSecret);
QByteArray buildStatusReport(const QString &clientId,
                             const QString &clientSecret);
TokenRequest authorizationCodeRequest(
    const QString &clientId,
    const QString &clientSecret,
    const QString &authorizationCode);
TokenRequest refreshTokenRequest(
    const QString &clientId,
    const QString &clientSecret,
    const QString &refreshToken);
RevocationRequest revocationRequest(
    const QString &clientId,
    const QString &clientSecret,
    const QString &token,
    RevocationTokenType tokenType);
QString tokenFailureMessage(
    int httpStatus,
    QNetworkReply::NetworkError networkError,
    const QString &networkErrorString,
    const QByteArray &payload,
    const QStringList &sensitiveValues);
QString revocationFailureMessage(
    int httpStatus,
    QNetworkReply::NetworkError networkError,
    const QString &networkErrorString,
    const QByteArray &payload,
    const QStringList &sensitiveValues);
TokenResponse parseTokenResponse(const QByteArray &payload);
TokenResponse parseAuthorizationResponse(
    const QByteArray &payload);

} // namespace StravaOAuthPolicy

#endif
