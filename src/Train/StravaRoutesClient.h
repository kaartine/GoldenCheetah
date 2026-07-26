/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_STRAVA_ROUTES_CLIENT_H
#define GC_STRAVA_ROUTES_CLIENT_H

#include "Cloud/StravaAuthenticatedSession.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

#include <functional>

struct StravaRouteSummary
{
    QString routeId;
    QString name;
    QString description;
};

class StravaRoutesClient final
{
public:
    static constexpr int PageSize = 100;
    static constexpr int MaximumPages = 100;
    static constexpr qsizetype MaximumJsonPayload =
        4 * 1024 * 1024;
    static constexpr qsizetype MaximumGpxPayload =
        32 * 1024 * 1024;

    struct RoutesResult {
        QList<StravaRouteSummary> routes;
        QString error;

        bool isValid() const
        {
            return error.isEmpty();
        }
    };

    struct PayloadResult {
        QByteArray payload;
        QString contentType;
        QString error;

        bool isValid() const
        {
            return error.isEmpty() && !payload.isEmpty();
        }
    };

    using CancellationCheck =
        StravaAuthenticatedSession::CancellationCheck;
    using AuthenticatedGet =
        std::function<StravaAuthenticatedSession::Result(
            const QUrl &url,
            qsizetype maximumBytes,
            const CancellationCheck &cancelled)>;

    explicit StravaRoutesClient(
        AuthenticatedGet authenticatedGet,
        QUrl apiBase = QUrl(
            QStringLiteral("https://www.strava.com/api/v3/")));

    RoutesResult listRoutes(
        const CancellationCheck &cancelled = {});
    PayloadResult downloadGpx(
        const QString &routeId,
        const CancellationCheck &cancelled = {});
    static QString workoutFileName(const QString &routeId);

private:
    StravaAuthenticatedSession::Result get(
        const QUrl &url,
        qsizetype maximumBytes,
        const CancellationCheck &cancelled) const;
    QUrl endpoint(const QString &relativePath) const;

    AuthenticatedGet authenticatedGet_;
    QUrl apiBase_;
};

#endif
