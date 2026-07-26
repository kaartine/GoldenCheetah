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
#include <QSet>

#include <algorithm>

namespace {

constexpr qsizetype MaximumClientIdLength = 20;
constexpr qsizetype MaximumOpaqueValueLength = 8 * 1024;
constexpr qsizetype MaximumErrorPayloadLength = 64 * 1024;
constexpr qsizetype MaximumScopeValueLength = 2 * 1024;
constexpr qsizetype MaximumScopeTokenLength = 128;
constexpr qsizetype MaximumProviderInputLength = 4 * 1024;
constexpr qsizetype MaximumProviderTextLength = 256;
constexpr qsizetype MaximumFailureMessageLength = 1024;
constexpr int MaximumProviderErrors = 3;
constexpr int MaximumGrantedScopes = 64;

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
    if (value.trimmed().compare(
            QStringLiteral("your_client_secret"),
            Qt::CaseInsensitive) == 0) {
        return true;
    }
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
    QStringList orderedValues = sensitiveValues;
    std::sort(
        orderedValues.begin(),
        orderedValues.end(),
        [](const QString &left, const QString &right) {
            return left.size() > right.size();
        });

    for (const QString &sensitive : orderedValues) {
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
                          Qt::CaseInsensitive);
        }
        const QString base64 = QString::fromLatin1(
            sensitive.toUtf8().toBase64());
        if (!base64.isEmpty() && base64 != sensitive) {
            value.replace(base64,
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

StravaOAuthPolicy::TokenResponse parseTokenPayload(
    const QByteArray &payload,
    QJsonObject *parsedObject)
{
    StravaOAuthPolicy::TokenResponse response;
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
    if (parsedObject) *parsedObject = object;
    return response;
}

bool isScopeTokenCharacter(const QChar character)
{
    const ushort code = character.unicode();
    return code == 0x21
        || (code >= 0x23 && code <= 0x5b)
        || (code >= 0x5d && code <= 0x7e);
}

bool parseGrantedScopes(
    const QJsonValue &value,
    QStringList &grantedScopes,
    QString &error)
{
    if (!value.isString()) {
        error = QStringLiteral(
            "Strava authorization did not return granted permissions.");
        return false;
    }

    const QString encoded = value.toString();
    if (encoded.trimmed().isEmpty()
        || encoded.size() > MaximumScopeValueLength) {
        error = QStringLiteral(
            "Strava returned invalid authorization permissions.");
        return false;
    }

    const QStringList rawScopes =
        encoded.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (rawScopes.isEmpty()
        || rawScopes.size() > MaximumGrantedScopes) {
        error = QStringLiteral(
            "Strava returned invalid authorization permissions.");
        return false;
    }

    QSet<QString> seen;
    for (const QString &scope : rawScopes) {
        if (scope.isEmpty()
            || scope.size() > MaximumScopeTokenLength) {
            error = QStringLiteral(
                "Strava returned invalid authorization permissions.");
            return false;
        }
        for (const QChar character : scope) {
            if (!isScopeTokenCharacter(character)) {
                error = QStringLiteral(
                    "Strava returned invalid authorization permissions.");
                return false;
            }
        }
        if (!seen.contains(scope)) {
            seen.insert(scope);
            grantedScopes.append(scope);
        }
    }
    grantedScopes.sort(Qt::CaseSensitive);

    QStringList missing;
    for (const QString &required : {
             QStringLiteral("read_all"),
             QStringLiteral("activity:read_all"),
             QStringLiteral("activity:write")}) {
        if (!seen.contains(required)) missing.append(required);
    }
    if (!missing.isEmpty()) {
        error = QStringLiteral(
            "Strava authorization is missing required permissions: %1.")
                    .arg(missing.join(QStringLiteral(", ")));
        grantedScopes.clear();
        return false;
    }
    return true;
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

QByteArray buildStatusReport(const QString &clientId,
                             const QString &clientSecret)
{
    QByteArray report = QByteArrayLiteral(
        "goldencheetah_build_status=1\n"
        "application=GoldenCheetah\n"
        "strava_support=enabled\n"
        "strava_oauth=");
    report += hasUsableCredentials(clientId, clientSecret)
        ? QByteArrayLiteral("configured\n")
        : QByteArrayLiteral("unavailable\n");
    return report;
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

RevocationRequest revocationRequest(
    const QString &clientId,
    const QString &clientSecret,
    const QString &token,
    RevocationTokenType tokenType)
{
    RevocationRequest request;
    if (!hasUsableCredentials(clientId, clientSecret)) {
        request.error = QStringLiteral(
            "Strava OAuth credentials are not configured in this build.");
        return request;
    }
    if (!isUsableOpaqueValue(token)) {
        request.error = QStringLiteral(
            "The Strava revocation token is missing or malformed.");
        return request;
    }

    QString tokenTypeHint;
    switch (tokenType) {
    case RevocationTokenType::AccessToken:
        tokenTypeHint = QStringLiteral("access_token");
        break;
    case RevocationTokenType::RefreshToken:
        tokenTypeHint = QStringLiteral("refresh_token");
        break;
    default:
        request.error = QStringLiteral(
            "The Strava revocation token type is invalid.");
        return request;
    }

    QByteArray basicCredentials = clientId.toUtf8();
    basicCredentials.append(':');
    basicCredentials.append(clientSecret.toUtf8());

    request.endpoint = QUrl(QStringLiteral(
        "https://www.strava.com/oauth/revoke"));
    request.method = QByteArrayLiteral("POST");
    request.authorizationHeader =
        QByteArrayLiteral("Basic ")
        + basicCredentials.toBase64();
    request.contentTypeHeader = QByteArrayLiteral(
        "application/x-www-form-urlencoded");
    request.body = formEncode({
        {QStringLiteral("token"), token},
        {QStringLiteral("token_type_hint"), tokenTypeHint}
    });
    return request;
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

QString revocationFailureMessage(
    int httpStatus,
    QNetworkReply::NetworkError networkError,
    const QString &networkErrorString,
    const QByteArray &payload,
    const QStringList &sensitiveValues)
{
    QString message;
    if (httpStatus >= 100 && httpStatus <= 599) {
        message = QStringLiteral(
            "Strava revocation request failed (HTTP %1).")
                      .arg(httpStatus);
    } else {
        QString transport = redactAndBound(
            networkErrorString, sensitiveValues);
        if (transport.isEmpty()) {
            transport = QStringLiteral("code %1")
                            .arg(static_cast<int>(networkError));
        }
        message = QStringLiteral(
            "Strava revocation request failed. Network error: %1.")
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
    return parseTokenPayload(payload, nullptr);
}

TokenResponse parseAuthorizationResponse(
    const QByteArray &payload)
{
    QJsonObject object;
    TokenResponse response =
        parseTokenPayload(payload, &object);
    if (!response.isValid()) return response;

    QString error;
    if (!parseGrantedScopes(
            object.value(QStringLiteral("scope")),
            response.grantedScopes,
            error)) {
        response.accessToken.clear();
        response.refreshToken.clear();
        response.grantedScopes.clear();
        response.error = error;
    }
    return response;
}

} // namespace StravaOAuthPolicy
