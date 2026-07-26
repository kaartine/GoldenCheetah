/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "StravaRevocationClient.h"

#include <QUrl>

#include <utility>

namespace {

constexpr qsizetype MaximumErrorLength = 1024;

bool cancellationRequested(
    const StravaRevocationClient::CancellationCheck &cancelled)
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

StravaRevocationClient::Result failure(
    StravaRevocationClient::Status status,
    const QString &error,
    int httpStatus = 0)
{
    StravaRevocationClient::Result result;
    result.status = status;
    result.httpStatus = httpStatus;
    result.error = boundedText(error);
    if (result.error.isEmpty()) {
        result.error = QStringLiteral(
            "The Strava revocation request failed.");
    }
    return result;
}

QStringList sensitiveValues(
    const QString &clientId,
    const QString &clientSecret,
    const QString &token,
    const StravaOAuthPolicy::RevocationRequest &request)
{
    const QString basicCredentials =
        QStringLiteral("%1:%2").arg(clientId, clientSecret);
    const QString basicPayload = QString::fromLatin1(
        basicCredentials.toUtf8().toBase64());
    return {
        token,
        clientSecret,
        basicCredentials,
        basicPayload,
        QString::fromLatin1(request.authorizationHeader)
    };
}

QString networkFailure(
    const StravaNetworkReply::Result &response,
    const QStringList &sensitive,
    bool includeHttpStatus)
{
    return StravaOAuthPolicy::revocationFailureMessage(
        includeHttpStatus ? response.httpStatus : 0,
        response.networkError,
        response.networkErrorString,
        response.payload,
        sensitive);
}

} // namespace

StravaRevocationClient::StravaRevocationClient(
    RequestOperation requestOperation)
    : requestOperation_(std::move(requestOperation))
{
}

StravaRevocationClient::Result
StravaRevocationClient::revoke(
    const QString &clientId,
    const QString &clientSecret,
    const QString &token,
    StravaOAuthPolicy::RevocationTokenType tokenType,
    const CancellationCheck &cancelled) const
{
    if (cancellationRequested(cancelled)) {
        return failure(
            Status::Cancelled,
            QStringLiteral(
                "The Strava revocation request was cancelled."));
    }
    if (!requestOperation_) {
        return failure(
            Status::InvalidRequest,
            QStringLiteral(
                "The Strava revocation transport is unavailable."));
    }

    const StravaOAuthPolicy::RevocationRequest request =
        StravaOAuthPolicy::revocationRequest(
            clientId, clientSecret, token, tokenType);
    if (!request.isValid()) {
        return failure(Status::InvalidRequest, request.error);
    }
    const QStringList sensitive = sensitiveValues(
        clientId, clientSecret, token, request);

    StravaNetworkReply::Result response;
    try {
        response = requestOperation_(
            request, MaximumResponseBytes, cancelled);
    } catch (...) {
        return failure(
            Status::TransportFailure,
            QStringLiteral(
                "The Strava revocation network operation failed."));
    }

    if (cancellationRequested(cancelled)) {
        return failure(
            Status::Cancelled,
            QStringLiteral(
                "The Strava revocation request was cancelled."),
            response.httpStatus);
    }

    switch (response.failure) {
    case StravaNetworkReply::Failure::None:
        break;
    case StravaNetworkReply::Failure::TimedOut:
        return failure(
            Status::TimedOut,
            QStringLiteral(
                "The Strava revocation request timed out."),
            response.httpStatus);
    case StravaNetworkReply::Failure::Cancelled:
        return failure(
            Status::Cancelled,
            QStringLiteral(
                "The Strava revocation request was cancelled."),
            response.httpStatus);
    case StravaNetworkReply::Failure::Oversized:
        return failure(
            Status::OversizedResponse,
            QStringLiteral(
                "Strava returned an oversized revocation response."),
            response.httpStatus);
    case StravaNetworkReply::Failure::Invalid:
        return failure(
            Status::InvalidResponse,
            QStringLiteral(
                "The Strava revocation network reply was invalid."),
            response.httpStatus);
    default:
        return failure(
            Status::InvalidResponse,
            QStringLiteral(
                "The Strava revocation network reply was invalid."),
            response.httpStatus);
    }

    if (response.networkError
            == QNetworkReply::OperationCanceledError) {
        return failure(
            Status::Cancelled,
            QStringLiteral(
                "The Strava revocation request was cancelled."),
            response.httpStatus);
    }
    if (response.networkError
            == QNetworkReply::TimeoutError) {
        return failure(
            Status::TimedOut,
            QStringLiteral(
                "The Strava revocation request timed out."),
            response.httpStatus);
    }
    if (response.payload.size() > MaximumResponseBytes) {
        return failure(
            Status::OversizedResponse,
            QStringLiteral(
                "Strava returned an oversized revocation response."),
            response.httpStatus);
    }

    const bool validHttpStatus =
        response.httpStatus >= 100
        && response.httpStatus <= 599;
    if (response.httpStatus != 200) {
        if (validHttpStatus) {
            return failure(
                Status::HttpFailure,
                networkFailure(response, sensitive, true),
                response.httpStatus);
        }
        if (response.networkError
                != QNetworkReply::NoError) {
            return failure(
                Status::TransportFailure,
                networkFailure(response, sensitive, false),
                response.httpStatus);
        }
        return failure(
            Status::InvalidResponse,
            QStringLiteral(
                "Strava returned an invalid revocation response."),
            response.httpStatus);
    }

    if (response.networkError != QNetworkReply::NoError) {
        return failure(
            Status::TransportFailure,
            networkFailure(response, sensitive, false),
            response.httpStatus);
    }

    Result result;
    result.status = Status::Succeeded;
    result.httpStatus = response.httpStatus;
    return result;
}
