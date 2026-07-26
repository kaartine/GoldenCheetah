/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "StravaNetworkReply.h"

#include "NetworkReplyWait.h"

#include <QNetworkRequest>
#include <QObject>
#include <QPointer>

#include <limits>
#include <utility>

namespace StravaNetworkReply {

namespace {

CancellationCheck safeCancellationCheck(
    const CancellationCheck &cancelled)
{
    if (!cancelled) return {};
    return [cancelled] {
        try {
            return cancelled();
        } catch (...) {
            return true;
        }
    };
}

} // namespace

Result collect(
    QNetworkReply *reply,
    qsizetype maximumBytes,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    Result result;
    QPointer<QNetworkReply> guardedReply(reply);
    const auto scheduleDeletion = [&] {
        if (guardedReply)
            guardedReply->deleteLater();
    };

    if (!guardedReply
        || maximumBytes <= 0
        || maximumBytes
            >= std::numeric_limits<qint64>::max()
        || timeoutMs <= 0) {
        result.failure = Failure::Invalid;
        result.networkError =
            QNetworkReply::UnknownNetworkError;
        result.networkErrorString = QStringLiteral(
            "The Strava network reply inputs are invalid.");
        scheduleDeletion();
        return result;
    }

    guardedReply->setReadBufferSize(maximumBytes + 1);
    QByteArray payload;
    bool oversized = false;
    QObject readContext;
    const auto appendAvailable = [&] {
        if (oversized || !guardedReply)
            return;
        const QByteArray chunk = guardedReply->readAll();
        if (payload.size() > maximumBytes
            || chunk.size() > maximumBytes - payload.size()) {
            oversized = true;
            payload.clear();
            guardedReply->abort();
            return;
        }
        payload.append(chunk);
    };
    QObject::connect(
        guardedReply, &QNetworkReply::readyRead,
        &readContext, appendAvailable);

    const NetworkReplyWaitResult waitResult =
        waitForNetworkReply(
            guardedReply,
            timeoutMs,
            safeCancellationCheck(cancelled));
    if (waitResult == NetworkReplyWaitResult::Finished)
        appendAvailable();

    if (!guardedReply) {
        result.failure = Failure::Invalid;
        result.networkError =
            QNetworkReply::UnknownNetworkError;
        result.networkErrorString = QStringLiteral(
            "The Strava network reply was destroyed early.");
        return result;
    }

    result.httpStatus = guardedReply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.networkError = guardedReply->error();
    result.networkErrorString =
        guardedReply->errorString();
    result.contentType = guardedReply->header(
        QNetworkRequest::ContentTypeHeader).toString();
    result.payload = std::move(payload);
    if (oversized) {
        result.failure = Failure::Oversized;
    } else if (
        waitResult == NetworkReplyWaitResult::TimedOut) {
        result.failure = Failure::TimedOut;
    } else if (
        waitResult == NetworkReplyWaitResult::Interrupted) {
        result.failure = Failure::Cancelled;
    }
    scheduleDeletion();
    return result;
}

} // namespace StravaNetworkReply
