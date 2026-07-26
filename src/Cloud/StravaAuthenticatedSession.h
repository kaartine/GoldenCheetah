/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_STRAVA_AUTHENTICATED_SESSION_H
#define GC_STRAVA_AUTHENTICATED_SESSION_H

#include "StravaNetworkReply.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <functional>

class StravaAuthenticatedSession final
{
public:
    struct Grant {
        QString accessToken;
        QString error;

        bool isValid() const
        {
            return !accessToken.isEmpty() && error.isEmpty();
        }
    };

    struct Result {
        QByteArray payload;
        QString contentType;
        QString error;
        int httpStatus = 0;

        bool isValid() const
        {
            return error.isEmpty();
        }
    };

    using CancellationCheck =
        StravaNetworkReply::CancellationCheck;
    using GrantProvider = std::function<Grant(
        const QString &rejectedAccessToken,
        const CancellationCheck &cancelled)>;
    using RequestOperation =
        std::function<StravaNetworkReply::Result(
            const QUrl &url,
            const QString &accessToken,
            qsizetype maximumBytes,
            const CancellationCheck &cancelled)>;

    StravaAuthenticatedSession(
        GrantProvider grantProvider,
        RequestOperation requestOperation);

    Result get(
        const QUrl &url,
        qsizetype maximumBytes,
        const CancellationCheck &cancelled = {});
    bool installGrant(const Grant &grant);
    void reset();

private:
    Grant acquireGrant(
        const QString &rejectedAccessToken,
        const CancellationCheck &cancelled);

    GrantProvider grantProvider_;
    RequestOperation requestOperation_;
    QString accessToken_;
};

#endif
