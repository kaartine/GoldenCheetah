/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDE_ITEM_REFRESH_RESULT_H
#define GC_RIDE_ITEM_REFRESH_RESULT_H

#include "RideCacheSnapshot.h"
#include "RideFileCache.h"
#include "RideFileCRC.h"

#include <QDateTime>
#include <QString>

class RideFile;

enum class RideItemRefreshOutcome
{
    AlreadyCurrent,
    Published,
    SourceFingerprintFailed,
    SourceOpenFailed,
    EnvironmentFingerprintUnavailable,
    EnvironmentMetricRegistryUnavailable,
    CachePreparationInvalid,
    IdentityRejected,
    CacheSourceRejected
};

constexpr bool rideItemRefreshSucceeded(RideItemRefreshOutcome outcome)
{
    return outcome == RideItemRefreshOutcome::AlreadyCurrent
        || outcome == RideItemRefreshOutcome::Published;
}

struct RideItemRefreshIdentity
{
    QString path;
    QString fileName;
    QDateTime dateTime;
    bool planned = false;
    bool open = false;
    RideFile *openRide = nullptr;
};

class RideItemRefreshGate
{
public:
    bool accepts(
        const RideItemRefreshIdentity &current,
        const QString &currentSourcePath,
        const RideFileCRC::ContentFingerprint &currentSource) const
    {
        return current.path == expected.path
            && current.fileName == expected.fileName
            && current.dateTime == expected.dateTime
            && current.planned == expected.planned
            && current.open == expected.open
            && (!expected.open
                || current.openRide == expected.openRide)
            && currentSourcePath == sourcePath
            && currentSource == sourceFingerprint;
    }

    RideItemRefreshIdentity expected;
    QString sourcePath;
    RideFileCRC::ContentFingerprint sourceFingerprint;
};

class RideItemRefreshResult : public RideItemRefreshGate
{
public:
    RideItemRefreshResult() = default;
    RideItemRefreshResult(RideItemRefreshResult &&) noexcept = default;
    RideItemRefreshResult &operator=(
        RideItemRefreshResult &&) noexcept = default;
    RideItemRefreshResult(const RideItemRefreshResult &) = delete;
    RideItemRefreshResult &operator=(
        const RideItemRefreshResult &) = delete;

    RideItemComputedState state;
    RideFileCache::PreparedRefresh cache;
};

#endif
