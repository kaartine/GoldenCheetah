/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "StravaApiReplyPolicy.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>

#include <cmath>

namespace {

constexpr qsizetype MaximumErrorPayloadSize = 64 * 1024;
constexpr qsizetype MaximumProviderInputLength = 4 * 1024;
constexpr qsizetype MaximumProviderTextLength = 256;
constexpr qsizetype MaximumFailureMessageLength = 1024;
constexpr qsizetype MaximumStreamTypeLength = 64;
constexpr int MaximumProviderErrors = 3;
constexpr int MaximumStreams = 64;

StravaApiReplyPolicy::Result failure(const QString &message)
{
    StravaApiReplyPolicy::Result result;
    result.error = message.left(MaximumFailureMessageLength);
    return result;
}

QString redactAndBound(
    QString value,
    const QStringList &sensitiveValues)
{
    for (const QString &sensitive : sensitiveValues) {
        if (sensitive.isEmpty()) {
            continue;
        }
        value.replace(
            sensitive, QStringLiteral("[redacted]"),
            Qt::CaseSensitive);
        const QString encoded = QString::fromLatin1(
            QUrl::toPercentEncoding(sensitive));
        if (encoded != sensitive) {
            value.replace(
                encoded, QStringLiteral("[redacted]"),
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
        || payload.size() > MaximumErrorPayloadSize) {
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
        if (!value.isString()) {
            continue;
        }
        message = redactAndBound(
            value.toString(), sensitiveValues);
        if (!message.isEmpty()) {
            break;
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

QString requestFailureMessage(
    int httpStatus,
    QNetworkReply::NetworkError networkError,
    const QString &networkErrorString,
    const QByteArray &payload,
    const QStringList &sensitiveValues)
{
    QString message;
    if (httpStatus >= 100 && httpStatus <= 599
        && (httpStatus < 200 || httpStatus >= 300)) {
        message = QStringLiteral(
            "Strava request failed (HTTP %1).")
                      .arg(httpStatus);
    } else {
        QString transport = redactAndBound(
            networkErrorString, sensitiveValues);
        if (transport.isEmpty()) {
            transport = QStringLiteral("code %1")
                            .arg(static_cast<int>(networkError));
        }
        message = QStringLiteral(
            "Strava request failed. Network error: %1.")
                      .arg(transport);
    }

    const QString provider =
        providerErrorSummary(payload, sensitiveValues);
    if (!provider.isEmpty()) {
        message.append(QLatin1Char(' '));
        message.append(provider);
        message.append(QLatin1Char('.'));
    }
    return message.left(MaximumFailureMessageLength);
}

bool hasJsonContentType(const QString &contentType)
{
    if (contentType.trimmed().isEmpty()) {
        return true;
    }

    const QString mediaType =
        contentType.section(QLatin1Char(';'), 0, 0)
            .trimmed()
            .toLower();
    return mediaType == QStringLiteral("application/json")
        || (mediaType.startsWith(QStringLiteral("application/"))
            && mediaType.endsWith(QStringLiteral("+json")));
}

bool isProviderFault(const QJsonDocument &document)
{
    if (!document.isObject()) {
        return false;
    }
    const QJsonObject object = document.object();
    return object.value(QStringLiteral("message")).isString()
        && object.value(QStringLiteral("errors")).isArray();
}

bool isKnownNumericStream(const QString &type)
{
    return type == QStringLiteral("time")
        || type == QStringLiteral("distance")
        || type == QStringLiteral("altitude")
        || type == QStringLiteral("velocity_smooth")
        || type == QStringLiteral("heartrate")
        || type == QStringLiteral("cadence")
        || type == QStringLiteral("watts")
        || type == QStringLiteral("temp");
}

bool isFiniteNumber(const QJsonValue &value)
{
    return value.isDouble()
        && std::isfinite(value.toDouble());
}

StravaApiReplyPolicy::Result validateActivity(
    const QJsonDocument &document)
{
    if (!document.isObject()) {
        return failure(QStringLiteral(
            "Strava returned an invalid activity response."));
    }

    const QJsonObject activity = document.object();
    const QJsonValue id = activity.value(QStringLiteral("id"));
    const double numericId = id.toDouble();
    if (!id.isDouble()
        || !std::isfinite(numericId)
        || numericId <= 0.0
        || std::floor(numericId) != numericId) {
        return failure(QStringLiteral(
            "Strava activity response is missing a valid activity ID."));
    }

    const QJsonValue start =
        activity.value(QStringLiteral("start_date_local"));
    if (!start.isString()
        || !QDateTime::fromString(
                start.toString(), Qt::ISODate).isValid()) {
        return failure(QStringLiteral(
            "Strava activity response is missing a valid start date."));
    }

    return {};
}

StravaApiReplyPolicy::Result validateStreams(
    const QJsonDocument &document)
{
    if (!document.isArray()) {
        return failure(QStringLiteral(
            "Strava returned an invalid streams response."));
    }

    const QJsonArray streams = document.array();
    if (streams.size() > MaximumStreams) {
        return failure(QStringLiteral(
            "Strava returned too many activity streams."));
    }

    StravaApiReplyPolicy::Result result;
    bool hasSampleCount = false;
    for (const QJsonValue &value : streams) {
        if (!value.isObject()) {
            return failure(QStringLiteral(
                "Strava returned an invalid activity stream."));
        }
        const QJsonObject stream = value.toObject();
        const QJsonValue type = stream.value(QStringLiteral("type"));
        const QJsonValue data = stream.value(QStringLiteral("data"));
        if (!type.isString()
            || type.toString().isEmpty()
            || type.toString().size() > MaximumStreamTypeLength
            || !data.isArray()) {
            return failure(QStringLiteral(
                "Strava returned an invalid activity stream."));
        }

        const QString streamType = type.toString();
        const QJsonArray samples = data.toArray();
        const qsizetype sampleCount = samples.size();
        if (!hasSampleCount) {
            result.sampleCount = sampleCount;
            hasSampleCount = true;
        } else if (sampleCount != result.sampleCount) {
            return failure(QStringLiteral(
                "Strava returned activity streams with mismatched lengths."));
        }

        if (streamType == QStringLiteral("latlng")) {
            for (const QJsonValue &sample : samples) {
                if (!sample.isArray()) {
                    return failure(QStringLiteral(
                        "Strava returned an invalid location stream."));
                }
                const QJsonArray coordinates = sample.toArray();
                if (coordinates.size() < 2
                    || !isFiniteNumber(coordinates.at(0))
                    || !isFiniteNumber(coordinates.at(1))) {
                    return failure(QStringLiteral(
                        "Strava returned an invalid location stream."));
                }
            }
        } else if (isKnownNumericStream(streamType)) {
            for (const QJsonValue &sample : samples) {
                if (!isFiniteNumber(sample)) {
                    return failure(QStringLiteral(
                        "Strava returned an invalid numeric stream."));
                }
            }
        }
    }
    return result;
}

} // namespace

namespace StravaApiReplyPolicy {

Result validate(
    PayloadKind kind,
    int httpStatus,
    QNetworkReply::NetworkError networkError,
    const QString &networkErrorString,
    const QByteArray &payload,
    const QStringList &sensitiveValues,
    const QString &contentType)
{
    if (networkError != QNetworkReply::NoError
        || httpStatus < 200
        || httpStatus >= 300) {
        return failure(requestFailureMessage(
            httpStatus,
            networkError,
            networkErrorString,
            payload,
            sensitiveValues));
    }

    if (payload.isEmpty()
        || payload.size() >= MaximumPayloadSize) {
        return failure(QStringLiteral(
            "Strava returned an empty or oversized response."));
    }

    if (!hasJsonContentType(contentType)) {
        return failure(QStringLiteral(
            "Strava returned an unexpected content type."));
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || document.isNull()) {
        return failure(QStringLiteral(
            "Strava returned malformed JSON."));
    }

    if (isProviderFault(document)) {
        QString message = QStringLiteral(
            "Strava returned an error response.");
        const QString provider =
            providerErrorSummary(payload, sensitiveValues);
        if (!provider.isEmpty()) {
            message.append(QLatin1Char(' '));
            message.append(provider);
            message.append(QLatin1Char('.'));
        }
        return failure(message);
    }

    return kind == PayloadKind::Activity
        ? validateActivity(document)
        : validateStreams(document);
}

} // namespace StravaApiReplyPolicy
