/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "StravaOAuthPolicy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QPair>

namespace {

constexpr qsizetype MaximumClientIdLength = 20;
constexpr qsizetype MaximumOpaqueValueLength = 8 * 1024;
constexpr qsizetype MaximumErrorPayloadLength = 64 * 1024;
constexpr qsizetype MaximumProviderInputLength = 4 * 1024;
constexpr qsizetype MaximumProviderTextLength = 256;
constexpr qsizetype MaximumFailureMessageLength = 1024;
constexpr int MaximumProviderErrors = 3;

bool isDecimalClientId(const QString &value)
{
    if (value.isEmpty()
        || value.size() > MaximumClientIdLength) {
        return false;
    }
    for (const QChar character : value) {
        if (character < QLatin1Char('0')
            || character > QLatin1Char('9')) {
            return false;
        }
    }
    return true;
}

bool isBuildPlaceholder(const QString &value)
{
    if (value.size() < 4
        || !value.startsWith(QStringLiteral("__"))
        || !value.endsWith(QStringLiteral("__"))) {
        return false;
    }

    for (qsizetype index = 2;
         index < value.size() - 2;
         ++index) {
        const QChar character = value.at(index);
        const bool allowed =
            (character >= QLatin1Char('A')
             && character <= QLatin1Char('Z'))
            || (character >= QLatin1Char('0')
                && character <= QLatin1Char('9'))
            || character == QLatin1Char('_');
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool isUsableOpaqueValue(const QString &value)
{
    if (value.trimmed().isEmpty()
        || value.size() > MaximumOpaqueValueLength) {
        return false;
    }
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (code < 0x20 || code == 0x7f) {
            return false;
        }
    }
    return true;
}

QByteArray formEncode(
    const QList<QPair<QString, QString>> &fields)
{
    QByteArray body;
    for (const auto &field : fields) {
        if (!body.isEmpty()) {
            body.append('&');
        }
        body.append(QUrl::toPercentEncoding(field.first));
        body.append('=');
        body.append(QUrl::toPercentEncoding(field.second));
    }
    return body;
}

StravaOAuthPolicy::TokenRequest makeTokenRequest(
    const QString &clientId,
    const QString &clientSecret,
    const QString &grantName,
    const QString &grantValue,
    const QString &grantType)
{
    StravaOAuthPolicy::TokenRequest request;
    if (!StravaOAuthPolicy::hasUsableCredentials(
            clientId, clientSecret)) {
        request.error = QStringLiteral(
            "Strava OAuth credentials are not configured in this build.");
        return request;
    }
    if (!isUsableOpaqueValue(grantValue)) {
        request.error = QStringLiteral(
            "The Strava OAuth grant is missing or malformed.");
        return request;
    }

    request.endpoint = QUrl(QStringLiteral(
        "https://www.strava.com/oauth/token"));
    request.body = formEncode({
        {QStringLiteral("client_id"), clientId},
        {QStringLiteral("client_secret"), clientSecret},
        {grantName, grantValue},
        {QStringLiteral("grant_type"), grantType}
    });
    return request;
}

QString redactAndBound(QString value,
                       const QStringList &sensitiveValues)
{
    for (const QString &sensitive : sensitiveValues) {
        if (sensitive.isEmpty()) {
            continue;
        }
        value.replace(sensitive,
                      QStringLiteral("[redacted]"),
                      Qt::CaseSensitive);
        const QString encoded = QString::fromLatin1(
            QUrl::toPercentEncoding(sensitive));
        if (encoded != sensitive) {
            value.replace(encoded,
                          QStringLiteral("[redacted]"),
                          Qt::CaseSensitive);
        }
    }
    value = value.left(MaximumProviderInputLength);

    for (QChar &character : value) {
        const ushort code = character.unicode();
        if (code < 0x20 || code == 0x7f) {
            character = QLatin1Char(' ');
        }
    }
    return value.simplified().left(MaximumProviderTextLength);
}

QString providerErrorSummary(
    const QByteArray &payload,
    const QStringList &sensitiveValues)
{
    if (payload.isEmpty()
        || payload.size() > MaximumErrorPayloadLength) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        return {};
    }

    const QJsonObject object = document.object();
    QString message;
    for (const QString &name : {
             QStringLiteral("message"),
             QStringLiteral("error_description"),
             QStringLiteral("error")}) {
        const QJsonValue value = object.value(name);
        if (value.isString() && !value.toString().isEmpty()) {
            message = redactAndBound(
                value.toString(), sensitiveValues);
            if (!message.isEmpty()) {
                break;
            }
        }
    }

    QStringList details;
    const QJsonArray errors =
        object.value(QStringLiteral("errors")).toArray();
    const int count = qMin(errors.size(), MaximumProviderErrors);
    for (int index = 0; index < count; ++index) {
        if (!errors.at(index).isObject()) {
            continue;
        }
        const QJsonObject error = errors.at(index).toObject();
        const QString resource = redactAndBound(
            error.value(QStringLiteral("resource")).toString(),
            sensitiveValues);
        const QString field = redactAndBound(
            error.value(QStringLiteral("field")).toString(),
            sensitiveValues);
        const QString code = redactAndBound(
            error.value(QStringLiteral("code")).toString(),
            sensitiveValues);

        QString location = resource;
        if (!field.isEmpty()) {
            if (!location.isEmpty()) {
                location.append(QLatin1Char('.'));
            }
            location.append(field);
        }
        if (!location.isEmpty() && !code.isEmpty()) {
            details.append(
                QStringLiteral("%1: %2").arg(location, code));
        } else if (!code.isEmpty()) {
            details.append(code);
        }
    }

    QStringList parts;
    if (!message.isEmpty()) {
        parts.append(message);
    }
    if (!details.isEmpty()) {
        parts.append(details.join(QStringLiteral(", ")));
    }
    return parts.join(QStringLiteral(". "));
}

} // namespace

namespace StravaOAuthPolicy {

bool hasUsableCredentials(const QString &clientId,
                          const QString &clientSecret)
{
    return isDecimalClientId(clientId)
        && isUsableOpaqueValue(clientSecret)
        && !isBuildPlaceholder(clientSecret);
}

TokenRequest authorizationCodeRequest(
    const QString &clientId,
    const QString &clientSecret,
    const QString &authorizationCode)
{
    return makeTokenRequest(
        clientId, clientSecret,
        QStringLiteral("code"), authorizationCode,
        QStringLiteral("authorization_code"));
}

TokenRequest refreshTokenRequest(
    const QString &clientId,
    const QString &clientSecret,
    const QString &refreshToken)
{
    return makeTokenRequest(
        clientId, clientSecret,
        QStringLiteral("refresh_token"), refreshToken,
        QStringLiteral("refresh_token"));
}

QString tokenFailureMessage(
    int httpStatus,
    QNetworkReply::NetworkError networkError,
    const QString &networkErrorString,
    const QByteArray &payload,
    const QStringList &sensitiveValues)
{
    QString message;
    if (httpStatus >= 100 && httpStatus <= 599) {
        message = QStringLiteral(
            "Strava token request failed (HTTP %1).")
                      .arg(httpStatus);
    } else {
        QString transport = redactAndBound(
            networkErrorString, sensitiveValues);
        if (transport.isEmpty()) {
            transport = QStringLiteral("code %1")
                            .arg(static_cast<int>(networkError));
        }
        message = QStringLiteral(
            "Strava token request failed. Network error: %1.")
                      .arg(transport);
    }

    const QString provider =
        providerErrorSummary(payload, sensitiveValues);
    if (!provider.isEmpty()) {
        message.append(QLatin1Char(' '));
        message.append(provider);
        message.append(QLatin1Char('.'));
    } else if (httpStatus >= 100
               && httpStatus <= 599) {
        const QString transport = redactAndBound(
            networkErrorString, sensitiveValues);
        if (!transport.isEmpty()) {
            message.append(QLatin1Char(' '));
            message.append(transport);
            message.append(QLatin1Char('.'));
        }
    }
    return message.left(MaximumFailureMessageLength);
}

TokenResponse parseTokenResponse(const QByteArray &payload)
{
    TokenResponse response;
    if (payload.isEmpty()
        || payload.size() > MaximumErrorPayloadLength) {
        response.error = QStringLiteral(
            "Strava returned an invalid token response.");
        return response;
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        response.error = QStringLiteral(
            "Strava returned an invalid token response.");
        return response;
    }

    const QJsonObject object = document.object();
    const QJsonValue access =
        object.value(QStringLiteral("access_token"));
    const QJsonValue refresh =
        object.value(QStringLiteral("refresh_token"));
    if (!access.isString() || !refresh.isString()
        || !isUsableOpaqueValue(access.toString())
        || !isUsableOpaqueValue(refresh.toString())) {
        response.error = QStringLiteral(
            "Strava token response is missing required tokens.");
        return response;
    }

    response.accessToken = access.toString();
    response.refreshToken = refresh.toString();
    return response;
}

} // namespace StravaOAuthPolicy
