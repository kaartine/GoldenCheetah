/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "StravaAuthenticatedSession.h"

#include <utility>

namespace {

constexpr qsizetype MaximumTokenLength = 8 * 1024;
constexpr qsizetype MaximumErrorLength = 1024;

bool cancellationRequested(
    const StravaAuthenticatedSession::CancellationCheck &cancelled)
{
    if (!cancelled) return false;
    try {
        return cancelled();
    } catch (...) {
        return true;
    }
}

bool isUsableToken(const QString &value)
{
    if (value.trimmed().isEmpty()
        || value.size() > MaximumTokenLength) {
        return false;
    }
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (code < 0x20 || code == 0x7f)
            return false;
    }
    return true;
}

bool isAllowedApiUrl(const QUrl &url)
{
    const int port = url.port(-1);
    const QString path = url.path();
    return url.isValid()
        && !url.isEmpty()
        && url.scheme().compare(
            QStringLiteral("https"),
            Qt::CaseInsensitive) == 0
        && url.host().compare(
            QStringLiteral("www.strava.com"),
            Qt::CaseInsensitive) == 0
        && url.userInfo().isEmpty()
        && (port == -1 || port == 443)
        && !url.hasFragment()
        && (path == QStringLiteral("/api/v3")
            || path.startsWith(
                QStringLiteral("/api/v3/")));
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

QString redact(
    QString value,
    const QString &accessToken)
{
    if (!accessToken.isEmpty()) {
        value.replace(
            accessToken,
            QStringLiteral("[redacted]"),
            Qt::CaseSensitive);
        const QString encoded = QString::fromLatin1(
            QUrl::toPercentEncoding(accessToken));
        if (encoded != accessToken) {
            value.replace(
                encoded,
                QStringLiteral("[redacted]"),
                Qt::CaseSensitive);
        }
    }
    return boundedText(value);
}

StravaAuthenticatedSession::Result failure(
    const QString &error,
    int httpStatus = 0)
{
    StravaAuthenticatedSession::Result result;
    result.error = boundedText(error);
    result.httpStatus = httpStatus;
    return result;
}

QString responseFailure(
    const StravaNetworkReply::Result &response,
    const QString &accessToken)
{
    if (response.httpStatus >= 100
        && response.httpStatus <= 599
        && (response.httpStatus < 200
            || response.httpStatus >= 300)) {
        return QStringLiteral(
            "Strava request failed (HTTP %1).")
                .arg(response.httpStatus);
    }

    QString detail = redact(
        response.networkErrorString, accessToken);
    if (detail.isEmpty()) {
        detail = QStringLiteral("code %1")
            .arg(static_cast<int>(response.networkError));
    }
    return boundedText(QStringLiteral(
        "Strava request failed. Network error: %1.")
            .arg(detail));
}

} // namespace

StravaAuthenticatedSession::StravaAuthenticatedSession(
    GrantProvider grantProvider,
    RequestOperation requestOperation)
    : grantProvider_(std::move(grantProvider)),
      requestOperation_(std::move(requestOperation))
{
}

void StravaAuthenticatedSession::reset()
{
    accessToken_.clear();
}

bool StravaAuthenticatedSession::installGrant(
    const Grant &grant)
{
    if (!grant.isValid()
        || !isUsableToken(grant.accessToken)) {
        return false;
    }
    accessToken_ = grant.accessToken;
    return true;
}

StravaAuthenticatedSession::Grant
StravaAuthenticatedSession::acquireGrant(
    const QString &rejectedAccessToken,
    const CancellationCheck &cancelled)
{
    Grant result;
    if (cancellationRequested(cancelled)) {
        result.error =
            QStringLiteral("Strava request was cancelled.");
        return result;
    }
    if (!grantProvider_) {
        result.error = QStringLiteral(
            "Strava credential provider is unavailable.");
        return result;
    }

    try {
        result = grantProvider_(
            rejectedAccessToken, cancelled);
    } catch (...) {
        result.error = QStringLiteral(
            "Strava credential refresh failed.");
    }
    const QString candidateAccessToken = result.accessToken;
    if (!result.isValid()
        || !isUsableToken(result.accessToken)) {
        result.accessToken.clear();
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "Strava credential refresh failed.");
        }
        result.error = redact(
            redact(result.error, rejectedAccessToken),
            candidateAccessToken);
    }
    return result;
}

StravaAuthenticatedSession::Result
StravaAuthenticatedSession::get(
    const QUrl &url,
    qsizetype maximumBytes,
    const CancellationCheck &cancelled)
{
    if (!isAllowedApiUrl(url)
        || maximumBytes <= 0
        || !requestOperation_) {
        return failure(QStringLiteral(
            "The authenticated Strava request is invalid."));
    }
    if (cancellationRequested(cancelled)) {
        return failure(
            QStringLiteral("Strava request was cancelled."));
    }

    if (accessToken_.isEmpty()) {
        const Grant initial =
            acquireGrant(QString(), cancelled);
        if (!initial.isValid())
            return failure(initial.error);
        accessToken_ = initial.accessToken;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (cancellationRequested(cancelled)) {
            return failure(
                QStringLiteral("Strava request was cancelled."));
        }

        const QString attemptAccessToken = accessToken_;
        StravaNetworkReply::Result response;
        try {
            response = requestOperation_(
                url,
                attemptAccessToken,
                maximumBytes,
                cancelled);
        } catch (...) {
            return failure(QStringLiteral(
                "The authenticated Strava network operation failed."));
        }

        if (response.failure
                == StravaNetworkReply::Failure::TimedOut) {
            return failure(
                QStringLiteral("Strava request timed out."));
        }
        if (response.failure
                == StravaNetworkReply::Failure::Cancelled
            || cancellationRequested(cancelled)) {
            return failure(
                QStringLiteral("Strava request was cancelled."));
        }
        if (response.failure
                == StravaNetworkReply::Failure::Oversized
            || response.payload.size() > maximumBytes) {
            return failure(QStringLiteral(
                "Strava returned an oversized response."));
        }
        if (response.failure
                == StravaNetworkReply::Failure::Invalid) {
            return failure(QStringLiteral(
                "The Strava network reply was invalid."));
        }

        if (response.httpStatus == 401 && attempt == 0) {
            if (accessToken_ != attemptAccessToken) {
                if (!isUsableToken(accessToken_)) {
                    return failure(QStringLiteral(
                        "Strava authorization changed during the request."));
                }
                continue;
            }
            const Grant refreshed =
                acquireGrant(attemptAccessToken, cancelled);
            if (!refreshed.isValid())
                return failure(refreshed.error);
            accessToken_ = refreshed.accessToken;
            continue;
        }

        if (response.networkError
                != QNetworkReply::NoError
            || response.httpStatus < 200
            || response.httpStatus >= 300) {
            return failure(
                responseFailure(
                    response, attemptAccessToken),
                response.httpStatus);
        }

        Result result;
        result.payload = std::move(response.payload);
        result.contentType = response.contentType;
        result.httpStatus = response.httpStatus;
        return result;
    }

    return failure(
        QStringLiteral("Strava authorization failed."));
}
