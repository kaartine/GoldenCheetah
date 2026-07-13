/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "CloudCredentialTransport.h"

#include <QJsonDocument>
#include <QUrlQuery>

namespace CloudCredentialTransport {

Request makeWithingsMeasuresRequest(const QUrl &endpoint,
                                    const QString &accessToken,
                                    const QDateTime &from,
                                    const QDateTime &to)
{
    QNetworkRequest request(endpoint);
    request.setRawHeader(
        QByteArrayLiteral("Authorization"),
        QByteArrayLiteral("Bearer ") + accessToken.toUtf8());
    request.setRawHeader(
        QByteArrayLiteral("Content-Type"),
        QByteArrayLiteral("application/x-www-form-urlencoded"));

    QUrlQuery parameters;
    parameters.addQueryItem(
        QStringLiteral("action"), QStringLiteral("getmeas"));
    parameters.addQueryItem(
        QStringLiteral("startdate"),
        QString::number(from.toSecsSinceEpoch()));
    parameters.addQueryItem(
        QStringLiteral("enddate"),
        QString::number(to.toSecsSinceEpoch()));

    return {
        request,
        parameters.query(QUrl::FullyEncoded).toUtf8()};
}

Request makeRideWithGpsAuthTokenRequest(const QUrl &endpoint,
                                        const QString &apiKey,
                                        const QString &email,
                                        const QString &password)
{
    QNetworkRequest request(endpoint);
    request.setRawHeader(
        QByteArrayLiteral("x-rwgps-api-key"),
        apiKey.toUtf8());
    request.setRawHeader(
        QByteArrayLiteral("Content-Type"),
        QByteArrayLiteral("application/json"));

    const QJsonObject user{
        {QStringLiteral("email"), email},
        {QStringLiteral("password"), password}};
    const QJsonObject root{
        {QStringLiteral("user"), user}};

    return {
        request,
        QJsonDocument(root).toJson(QJsonDocument::Compact)};
}

QString rideWithGpsAuthToken(const QJsonObject &response)
{
    const QJsonValue current =
        response.value(QStringLiteral("auth_token"));
    if (current.isString()) {
        return current.toString();
    }

    const QString token = current.toObject()
        .value(QStringLiteral("auth_token")).toString();
    if (!token.isEmpty()) {
        return token;
    }

    return response.value(QStringLiteral("user")).toObject()
        .value(QStringLiteral("auth_token")).toString();
}

} // namespace CloudCredentialTransport
