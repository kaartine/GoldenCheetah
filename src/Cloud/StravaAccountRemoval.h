/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_STRAVA_ACCOUNT_REMOVAL_H
#define GC_STRAVA_ACCOUNT_REMOVAL_H

#include <QString>

#include <cstdint>
#include <functional>

namespace StravaAccountRemoval {

enum class Mode
{
    RevokeRemote,
    LocalOnly
};

enum class RevocationToken
{
    AccessToken,
    RefreshToken
};

struct RemoteRevocationResult
{
    bool revoked = false;
    QString error;

    bool isSuccess() const
    {
        return revoked && error.isEmpty();
    }
};

struct Request
{
    QString accountKey;
    QString accessToken;
    QString refreshToken;
    QString clientId;
    QString clientSecret;
    Mode mode = Mode::RevokeRemote;
    std::uint64_t expectedAuthorizationEpoch = 0;

    bool isValid() const;
};

struct Result
{
    bool disconnected = false;
    bool cleanupPending = false;
    bool remoteAuthorizationMayRemain = true;
    QString error;

    bool isSuccess() const
    {
        return disconnected && error.isEmpty();
    }
};

using CancellationCheck = std::function<bool()>;
using IrreversibleCallback = std::function<void()>;
using RemoteRevocationOperation =
    std::function<RemoteRevocationResult(
        const QString &token,
        RevocationToken tokenType,
        const CancellationCheck &cancelled)>;

Result execute(
    const Request &request,
    const CancellationCheck &cancelled = {},
    const IrreversibleCallback &irreversible = {});
Result execute(
    const Request &request,
    const CancellationCheck &cancelled,
    const RemoteRevocationOperation &revokeRemote,
    const IrreversibleCallback &irreversible = {});

} // namespace StravaAccountRemoval

#endif
