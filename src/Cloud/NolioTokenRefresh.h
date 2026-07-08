/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_NOLIO_TOKEN_REFRESH_H
#define GC_NOLIO_TOKEN_REFRESH_H

#include <QString>

#include <chrono>
#include <functional>

struct NolioTokenRefreshResult
{
    bool success = false;
    QString accessToken;
    QString refreshToken;
    QString refreshedAt;
    QString error;
};

class NolioTokenRefreshCoordinator final
{
public:
    using RefreshOperation =
            std::function<NolioTokenRefreshResult()>;
    using CancellationCheck = std::function<bool()>;

    static NolioTokenRefreshResult refresh(
        const QString &inputRefreshToken,
        const RefreshOperation &operation,
        const CancellationCheck &cancelled = {});
    static NolioTokenRefreshResult refresh(
        const QString &inputRefreshToken,
        const RefreshOperation &operation,
        const CancellationCheck &cancelled,
        std::chrono::milliseconds cacheLifetime);

private:
    NolioTokenRefreshCoordinator() = delete;
};

#endif
