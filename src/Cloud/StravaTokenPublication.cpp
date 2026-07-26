/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "StravaTokenPublication.h"

namespace StravaTokenPublication {

namespace {

PublicationResult invalidInput()
{
    return {
        PublicationStatus::InvalidInput,
        QStringLiteral("Strava token publication inputs are invalid.")
    };
}

PublicationResult storageFailure()
{
    return {
        PublicationStatus::StorageFailure,
        QStringLiteral("Strava credentials could not be stored securely.")
    };
}

} // namespace

PublicationResult publish(
    const QString &expectedRefreshToken,
    const TokenPair &replacement,
    const QString &refreshedAt,
    PublicationMode mode,
    const PublicationCallbacks &callbacks)
{
    if (!replacement.isValid()
        || refreshedAt.isEmpty()
        || !callbacks.isValid()
        || (mode == PublicationMode::CompareAndSwap
            && expectedRefreshToken.isEmpty())) {
        return invalidInput();
    }

    try {
        const TokenPair current = callbacks.readCurrent();
        const bool alreadyCurrent =
            current.accessToken == replacement.accessToken
            && current.refreshToken == replacement.refreshToken;
        if (mode == PublicationMode::CompareAndSwap
            && current.refreshToken != expectedRefreshToken
            && current.refreshToken != replacement.refreshToken) {
            return {
                PublicationStatus::Conflict,
                QStringLiteral(
                    "Newer Strava credentials are already configured.")
            };
        }

        if (!callbacks.writeRefreshToken(
                replacement.refreshToken)) {
            return storageFailure();
        }
        if (!callbacks.writeAccessToken(
                replacement.accessToken)) {
            return storageFailure();
        }
        if (!callbacks.writeTimestamp(refreshedAt))
            return storageFailure();

        return {
            alreadyCurrent
                ? PublicationStatus::AlreadyCurrent
                : PublicationStatus::Saved,
            QString()
        };
    } catch (...) {
        return storageFailure();
    }
}

} // namespace StravaTokenPublication
