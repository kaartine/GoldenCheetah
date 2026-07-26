/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_STRAVA_NETWORK_REPLY_H
#define GC_STRAVA_NETWORK_REPLY_H

#include <QByteArray>
#include <QNetworkReply>
#include <QString>

#include <functional>

namespace StravaNetworkReply {

enum class Failure {
    None,
    TimedOut,
    Cancelled,
    Oversized,
    Invalid
};

struct Result {
    int httpStatus = 0;
    QNetworkReply::NetworkError networkError =
        QNetworkReply::NoError;
    QString networkErrorString;
    QByteArray payload;
    QString contentType;
    Failure failure = Failure::None;
};

using CancellationCheck = std::function<bool()>;

Result collect(
    QNetworkReply *reply,
    qsizetype maximumBytes,
    int timeoutMs,
    const CancellationCheck &cancelled = {});

} // namespace StravaNetworkReply

Q_DECLARE_METATYPE(StravaNetworkReply::Failure)

#endif
