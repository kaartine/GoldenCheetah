/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "StravaRoutesClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <cmath>
#include <utility>

namespace {

constexpr qsizetype MaximumErrorLength = 1024;
constexpr qsizetype MaximumRouteNameLength = 1024;
constexpr qsizetype MaximumRouteDescriptionLength = 16 * 1024;
constexpr double MaximumExactJsonInteger = 9007199254740991.0;

bool cancellationRequested(
    const StravaRoutesClient::CancellationCheck &cancelled)
{
    if (!cancelled) return false;
    try {
        return cancelled();
    } catch (...) {
        return true;
    }
}

QString boundedText(QString value)
{
    value = value.left(MaximumErrorLength);
    for (QChar &character : value) {
        const ushort code = character.unicode();
        if (code < 0x20 || code == 0x7f)
            character = QLatin1Char(' ');
    }
    return value.simplified();
}

QString decimalId(const QJsonValue &value)
{
    if (value.isString()) {
        const QString text = value.toString();
        if (text.isEmpty() || text.size() > 19)
            return {};
        for (const QChar character : text) {
            if (character < QLatin1Char('0')
                || character > QLatin1Char('9')) {
                return {};
            }
        }
        bool ok = false;
        const qlonglong parsed = text.toLongLong(&ok);
        if (!ok || parsed <= 0)
            return {};
        return text;
    }

    if (!value.isDouble())
        return {};
    const double number = value.toDouble();
    if (!std::isfinite(number)
        || number <= 0.0
        || number > MaximumExactJsonInteger
        || std::floor(number) != number) {
        return {};
    }
    return QString::number(number, 'f', 0);
}

bool hasJsonContentType(const QString &contentType)
{
    if (contentType.trimmed().isEmpty())
        return true;
    const QString mediaType =
        contentType.section(QLatin1Char(';'), 0, 0)
            .trimmed().toLower();
    return mediaType == QStringLiteral("application/json")
        || (mediaType.startsWith(QStringLiteral("application/"))
            && mediaType.endsWith(QStringLiteral("+json")));
}

bool hasGpxContentType(const QString &contentType)
{
    if (contentType.trimmed().isEmpty())
        return true;
    const QString mediaType =
        contentType.section(QLatin1Char(';'), 0, 0)
            .trimmed().toLower();
    return mediaType == QStringLiteral("application/gpx+xml")
        || mediaType == QStringLiteral("application/xml")
        || mediaType == QStringLiteral("text/xml")
        || mediaType == QStringLiteral("application/octet-stream");
}

bool isValidGpx(const QByteArray &payload)
{
    QXmlStreamReader reader(payload);
    bool sawRoot = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.tokenType()
            == QXmlStreamReader::DTD) {
            return false;
        }
        if (!sawRoot && reader.isStartElement()) {
            sawRoot =
                reader.name() == QStringLiteral("gpx");
            if (!sawRoot)
                return false;
        }
    }
    return sawRoot && !reader.hasError();
}

StravaRoutesClient::PayloadResult payloadFailure(
    const QString &error)
{
    StravaRoutesClient::PayloadResult result;
    result.error = boundedText(error);
    return result;
}

StravaRoutesClient::RoutesResult routesFailure(
    const QString &error)
{
    StravaRoutesClient::RoutesResult result;
    result.error = boundedText(error);
    return result;
}

} // namespace

StravaRoutesClient::StravaRoutesClient(
    AuthenticatedGet authenticatedGet,
    QUrl apiBase)
    : authenticatedGet_(std::move(authenticatedGet)),
      apiBase_(std::move(apiBase))
{
    if (!apiBase_.path().endsWith(QLatin1Char('/'))) {
        apiBase_.setPath(
            apiBase_.path() + QLatin1Char('/'));
    }
}

QUrl StravaRoutesClient::endpoint(
    const QString &relativePath) const
{
    if (!apiBase_.isValid()
        || apiBase_.scheme() != QStringLiteral("https")
        || apiBase_.host().isEmpty()) {
        return {};
    }
    return apiBase_.resolved(QUrl(relativePath));
}

StravaAuthenticatedSession::Result
StravaRoutesClient::get(
    const QUrl &url,
    qsizetype maximumBytes,
    const CancellationCheck &cancelled) const
{
    if (!url.isValid()
        || url.isEmpty()
        || maximumBytes <= 0
        || !authenticatedGet_) {
        StravaAuthenticatedSession::Result result;
        result.error = QStringLiteral(
            "Strava Routes request configuration is invalid.");
        return result;
    }
    if (cancellationRequested(cancelled)) {
        StravaAuthenticatedSession::Result result;
        result.error =
            QStringLiteral("Strava Routes request was cancelled.");
        return result;
    }

    StravaAuthenticatedSession::Result result;
    try {
        result = authenticatedGet_(
            url, maximumBytes, cancelled);
    } catch (...) {
        result.error = QStringLiteral(
            "Strava Routes request failed.");
    }
    if (!result.isValid()) {
        result.payload.clear();
        if (result.error.isEmpty()) {
            result.error =
                QStringLiteral("Strava Routes request failed.");
        }
        result.error = boundedText(result.error);
        return result;
    }
    if (result.payload.size() > maximumBytes) {
        result.payload.clear();
        result.contentType.clear();
        result.error = QStringLiteral(
            "Strava returned an oversized Routes response.");
    }
    return result;
}

StravaRoutesClient::RoutesResult
StravaRoutesClient::listRoutes(
    const CancellationCheck &cancelled)
{
    const StravaAuthenticatedSession::Result athleteResponse =
        get(endpoint(QStringLiteral("athlete")),
            MaximumJsonPayload, cancelled);
    if (!athleteResponse.isValid())
        return routesFailure(athleteResponse.error);
    if (athleteResponse.payload.isEmpty()
        || !hasJsonContentType(athleteResponse.contentType)) {
        return routesFailure(QStringLiteral(
            "Strava returned an invalid athlete response."));
    }

    QJsonParseError parseError;
    const QJsonDocument athleteDocument =
        QJsonDocument::fromJson(
            athleteResponse.payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !athleteDocument.isObject()) {
        return routesFailure(QStringLiteral(
            "Strava returned an invalid athlete response."));
    }
    const QJsonObject athlete = athleteDocument.object();
    QString athleteId =
        decimalId(athlete.value(QStringLiteral("id_str")));
    if (athleteId.isEmpty()) {
        athleteId =
            decimalId(athlete.value(QStringLiteral("id")));
    }
    if (athleteId.isEmpty()) {
        return routesFailure(QStringLiteral(
            "Strava athlete response is missing a valid ID."));
    }

    RoutesResult result;
    QSet<QString> routeIds;
    for (int page = 1; page <= MaximumPages; ++page) {
        if (cancellationRequested(cancelled)) {
            return routesFailure(QStringLiteral(
                "Strava Routes request was cancelled."));
        }

        QUrl routesUrl = endpoint(QStringLiteral(
            "athletes/%1/routes").arg(athleteId));
        QUrlQuery query;
        query.addQueryItem(
            QStringLiteral("per_page"),
            QString::number(PageSize));
        query.addQueryItem(
            QStringLiteral("page"),
            QString::number(page));
        routesUrl.setQuery(query);

        const StravaAuthenticatedSession::Result pageResponse =
            get(routesUrl, MaximumJsonPayload, cancelled);
        if (!pageResponse.isValid())
            return routesFailure(pageResponse.error);
        if (pageResponse.payload.isEmpty()
            || !hasJsonContentType(pageResponse.contentType)) {
            return routesFailure(QStringLiteral(
                "Strava returned an invalid Routes list."));
        }

        const QJsonDocument routesDocument =
            QJsonDocument::fromJson(
                pageResponse.payload, &parseError);
        if (parseError.error != QJsonParseError::NoError
            || !routesDocument.isArray()) {
            return routesFailure(QStringLiteral(
                "Strava returned an invalid Routes list."));
        }

        const QJsonArray routes = routesDocument.array();
        if (routes.size() > PageSize) {
            return routesFailure(QStringLiteral(
                "Strava returned too many Routes in one page."));
        }
        for (const QJsonValue &value : routes) {
            if (!value.isObject()) {
                return routesFailure(QStringLiteral(
                    "Strava returned an invalid Route entry."));
            }
            const QJsonObject object = value.toObject();
            QString routeId =
                decimalId(object.value(
                    QStringLiteral("id_str")));
            if (routeId.isEmpty()) {
                routeId = decimalId(object.value(
                    QStringLiteral("id")));
            }
            const QJsonValue nameValue =
                object.value(QStringLiteral("name"));
            const QJsonValue descriptionValue =
                object.value(QStringLiteral("description"));
            if (routeId.isEmpty()
                || !nameValue.isString()
                || nameValue.toString().isEmpty()
                || nameValue.toString().size()
                    > MaximumRouteNameLength
                || (!descriptionValue.isUndefined()
                    && !descriptionValue.isNull()
                    && !descriptionValue.isString())
                || descriptionValue.toString().size()
                    > MaximumRouteDescriptionLength) {
                return routesFailure(QStringLiteral(
                    "Strava returned an invalid Route entry."));
            }
            if (routeIds.contains(routeId)) {
                return routesFailure(QStringLiteral(
                    "Strava returned duplicate Route IDs."));
            }
            routeIds.insert(routeId);
            result.routes.append({
                routeId,
                nameValue.toString(),
                descriptionValue.toString()
            });
        }

        if (routes.size() < PageSize)
            return result;
    }

    return routesFailure(QStringLiteral(
        "Strava Routes pagination exceeded its safety limit."));
}

StravaRoutesClient::PayloadResult
StravaRoutesClient::downloadGpx(
    const QString &routeId,
    const CancellationCheck &cancelled)
{
    if (decimalId(QJsonValue(routeId)).isEmpty()) {
        return payloadFailure(
            QStringLiteral("The Strava Route ID is invalid."));
    }

    const StravaAuthenticatedSession::Result response =
        get(endpoint(QStringLiteral(
                "routes/%1/export_gpx").arg(routeId)),
            MaximumGpxPayload, cancelled);
    if (!response.isValid())
        return payloadFailure(response.error);
    if (response.payload.isEmpty()
        || !hasGpxContentType(response.contentType)
        || !isValidGpx(response.payload)) {
        return payloadFailure(QStringLiteral(
            "Strava returned an invalid GPX response."));
    }

    PayloadResult result;
    result.payload = response.payload;
    result.contentType = response.contentType;
    return result;
}

QString StravaRoutesClient::workoutFileName(
    const QString &routeId)
{
    if (decimalId(QJsonValue(routeId)).isEmpty())
        return {};
    return QStringLiteral("Strava-Route-%1.gpx")
        .arg(routeId);
}
