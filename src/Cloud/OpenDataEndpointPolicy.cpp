/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "OpenDataEndpointPolicy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace {

constexpr int MaxDiscoveryBytes = 64 * 1024;
constexpr int MaxDiscoveredServers = 32;
constexpr int AllowedServerPort = 9090;
constexpr int TransferTimeoutMs = 30 * 1000;

const QString AllowedServerHost = QStringLiteral("liversedge.ddns.net");

} // namespace

namespace OpenDataEndpointPolicy {

QUrl discoveryUrl()
{
    return QUrl(QStringLiteral(
        "https://www.goldencheetah.org/opendata.json"), QUrl::StrictMode);
}

bool isAllowedServerRoot(const QUrl &url)
{
    if (!url.isValid() || url.isRelative()
        || url.scheme().compare(QStringLiteral("https"),
                                Qt::CaseInsensitive) != 0
        || url.host().compare(AllowedServerHost, Qt::CaseInsensitive) != 0
        || url.port(-1) != AllowedServerPort
        || url.path(QUrl::FullyEncoded) != QStringLiteral("/")
        || url.hasQuery() || url.hasFragment()
        || !url.userName().isEmpty() || !url.password().isEmpty()
        || url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@'))) {
        return false;
    }

    return true;
}

QList<QUrl> allowedServerRootsFromDiscovery(const QByteArray &response,
                                            QString *error)
{
    if (error) {
        error->clear();
    }

    if (response.size() > MaxDiscoveryBytes) {
        if (error) {
            *error = QStringLiteral("OpenData discovery response is too large.");
        }
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("Invalid OpenData discovery response.");
        }
        return {};
    }

    const QJsonValue serversValue =
        document.object().value(QStringLiteral("SERVERS"));
    if (!serversValue.isArray()) {
        if (error) {
            *error = QStringLiteral(
                "OpenData discovery response has no SERVERS array.");
        }
        return {};
    }

    const QJsonArray results = serversValue.toArray();
    if (results.size() > MaxDiscoveredServers) {
        if (error) {
            *error = QStringLiteral(
                "OpenData discovery response has too many servers.");
        }
        return {};
    }

    QList<QUrl> servers;
    for (const QJsonValue &value : results) {
        if (!value.isObject()) {
            continue;
        }

        const QJsonValue urlValue =
            value.toObject().value(QStringLiteral("url"));
        if (!urlValue.isString()) {
            continue;
        }

        const QUrl url(urlValue.toString(), QUrl::StrictMode);
        if (isAllowedServerRoot(url) && !servers.contains(url)) {
            servers.append(url);
        }
    }

    return servers;
}

QUrl metricsUrl(const QUrl &serverRoot)
{
    if (!isAllowedServerRoot(serverRoot)) {
        return {};
    }

    QUrl url(serverRoot);
    url.setPath(QStringLiteral("/metrics"));
    return url;
}

QNetworkRequest makeRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(TransferTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    return request;
}

bool isSuccessfulResponse(QNetworkReply::NetworkError error,
                          int httpStatus,
                          const QUrl &redirectTarget)
{
    return error == QNetworkReply::NoError
        && httpStatus >= 200
        && httpStatus < 300
        && redirectTarget.isEmpty();
}

} // namespace OpenDataEndpointPolicy
