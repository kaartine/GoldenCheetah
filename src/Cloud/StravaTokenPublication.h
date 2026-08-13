/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_STRAVA_TOKEN_PUBLICATION_H
#define GC_STRAVA_TOKEN_PUBLICATION_H

#include <QString>

#include <functional>

namespace StravaTokenPublication {

struct TokenPair
{
    QString accessToken;
    QString refreshToken;

    bool isValid() const
    {
        return !accessToken.isEmpty() && !refreshToken.isEmpty();
    }
};

enum class PublicationMode
{
    CompareAndSwap,
    Authoritative
};

enum class PublicationStatus
{
    Saved,
    AlreadyCurrent,
    Pending,
    Conflict,
    StorageFailure,
    InvalidInput
};

struct PublicationResult
{
    PublicationStatus status = PublicationStatus::InvalidInput;
    QString error;

    bool isSuccess() const
    {
        return status == PublicationStatus::Saved
            || status == PublicationStatus::AlreadyCurrent;
    }
};

enum class RemovalStatus
{
    Cleared,
    CleanupPending,
    Pending,
    Conflict,
    StorageFailure,
    InvalidInput
};

struct RemovalResult
{
    RemovalStatus status = RemovalStatus::InvalidInput;
    QString error;

    bool isSuccess() const
    {
        return status == RemovalStatus::Cleared
            || status == RemovalStatus::CleanupPending;
    }
};

struct PublicationCallbacks
{
    std::function<TokenPair()> readCurrent;
    std::function<bool(const QString &)> writeRefreshToken;
    std::function<bool(const QString &)> writeAccessToken;
    std::function<bool(const QString &)> writeTimestamp;

    bool isValid() const
    {
        return readCurrent
            && writeRefreshToken
            && writeAccessToken
            && writeTimestamp;
    }
};

PublicationResult publish(
    const QString &expectedRefreshToken,
    const TokenPair &replacement,
    const QString &refreshedAt,
    PublicationMode mode,
    const PublicationCallbacks &callbacks);

RemovalResult remove(
    const QString &expectedRefreshToken,
    PublicationMode mode,
    const PublicationCallbacks &callbacks);

} // namespace StravaTokenPublication

#endif
