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

constexpr int MaximumErrorLength = 1024;

QString boundedError(const QString &error)
{
    return error.left(MaximumErrorLength);
}

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

RemovalResult invalidRemovalInput()
{
    return {
        RemovalStatus::InvalidInput,
        boundedError(QStringLiteral(
            "Strava credential removal inputs are invalid."))
    };
}

RemovalResult removalConflict()
{
    return {
        RemovalStatus::Conflict,
        boundedError(QStringLiteral(
            "Newer Strava credentials are already configured."))
    };
}

RemovalResult removalStorageFailure()
{
    return {
        RemovalStatus::StorageFailure,
        boundedError(QStringLiteral(
            "Strava credentials could not be removed securely."))
    };
}

RemovalResult cleanupPending()
{
    return {
        RemovalStatus::CleanupPending,
        boundedError(QStringLiteral(
            "Strava credentials were removed, but secure storage "
            "cleanup is still pending."))
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

RemovalResult remove(
    const QString &expectedRefreshToken,
    PublicationMode mode,
    const PublicationCallbacks &callbacks)
{
    const bool supportedMode =
        mode == PublicationMode::CompareAndSwap
        || mode == PublicationMode::Authoritative;
    if (!supportedMode
        || !callbacks.isValid()
        || (mode == PublicationMode::CompareAndSwap
            && expectedRefreshToken.isEmpty())) {
        return invalidRemovalInput();
    }

    TokenPair current;
    try {
        current = callbacks.readCurrent();
    } catch (...) {
        return removalStorageFailure();
    }

    if (mode == PublicationMode::CompareAndSwap
        && !current.refreshToken.isEmpty()
        && current.refreshToken != expectedRefreshToken) {
        return removalConflict();
    }

    bool physicalFailure = false;
    try {
        physicalFailure =
            !callbacks.writeRefreshToken(QString());
    } catch (...) {
        physicalFailure = true;
    }
    try {
        if (!callbacks.writeAccessToken(QString()))
            physicalFailure = true;
    } catch (...) {
        physicalFailure = true;
    }
    try {
        if (!callbacks.writeTimestamp(QString()))
            physicalFailure = true;
    } catch (...) {
        physicalFailure = true;
    }

    TokenPair remaining;
    try {
        remaining = callbacks.readCurrent();
    } catch (...) {
        return removalStorageFailure();
    }

    if (!remaining.accessToken.isEmpty()
        || !remaining.refreshToken.isEmpty()) {
        return removalStorageFailure();
    }
    if (physicalFailure)
        return cleanupPending();

    return {
        RemovalStatus::Cleared,
        QString()
    };
}

} // namespace StravaTokenPublication
