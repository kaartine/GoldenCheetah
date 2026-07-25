/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_StravaApiReplyPolicy_h
#define GC_StravaApiReplyPolicy_h

#include <QByteArray>
#include <QNetworkReply>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace StravaApiReplyPolicy {

inline constexpr qsizetype MaximumPayloadSize = 4 * 1024 * 1024;

enum class PayloadKind {
    Activity,
    Streams
};

struct Result {
    QString error;
    qsizetype sampleCount = 0;

    bool isValid() const
    {
        return error.isEmpty();
    }
};

Result validate(
    PayloadKind kind,
    int httpStatus,
    QNetworkReply::NetworkError networkError,
    const QString &networkErrorString,
    const QByteArray &payload,
    const QStringList &sensitiveValues = {},
    const QString &contentType = {});

} // namespace StravaApiReplyPolicy

#endif
