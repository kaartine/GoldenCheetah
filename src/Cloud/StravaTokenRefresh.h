/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_STRAVA_TOKEN_REFRESH_H
#define GC_STRAVA_TOKEN_REFRESH_H

#include <QString>

#include <chrono>
#include <functional>

struct StravaTokenRefreshResult
{
    bool success = false;
    QString accessToken;
    QString refreshToken;
    QString error;

    bool isValid() const;
};

class StravaTokenRefreshCoordinator final
{
public:
    using RefreshOperation =
        std::function<StravaTokenRefreshResult()>;
    using CanonicalRefreshOperation =
        std::function<StravaTokenRefreshResult(const QString &)>;
    using CancellationCheck = std::function<bool()>;

    static StravaTokenRefreshResult refresh(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const RefreshOperation &operation,
        const CancellationCheck &cancelled = {});
    static StravaTokenRefreshResult refresh(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const RefreshOperation &operation,
        const CancellationCheck &cancelled,
        std::chrono::milliseconds cacheLifetime);

    static StravaTokenRefreshResult refresh(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const CanonicalRefreshOperation &operation,
        const CancellationCheck &cancelled = {});
    static StravaTokenRefreshResult refresh(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const CanonicalRefreshOperation &operation,
        const CancellationCheck &cancelled,
        std::chrono::milliseconds cacheLifetime);

    static bool installAuthorization(
        const QString &accountKey,
        const StravaTokenRefreshResult &authorization);
    static void invalidate(const QString &accountKey);

private:
    StravaTokenRefreshCoordinator() = delete;
};

#endif
