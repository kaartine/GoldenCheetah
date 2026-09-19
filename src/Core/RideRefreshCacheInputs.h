/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDEREFRESHCACHEINPUTS_H
#define GC_RIDEREFRESHCACHEINPUTS_H

#include "RideRefreshZones.h"

#include <QByteArray>
#include <QDate>
#include <QString>

#include <optional>

struct RideRefreshCacheSettings {
    QString wbalFormula = QStringLiteral("int");
    int wbalTau = 300;
    int wheelSize = 2100;
};

struct RideRefreshDistributionZones {
    bool valid = false;
    bool powerDomainPresent = false;
    bool heartRateDomainPresent = false;
    bool paceDomainPresent = false;
    std::optional<RideRefreshZones::PowerRange> power;
    std::optional<RideRefreshZones::HeartRateRange> heartRate;
    std::optional<RideRefreshZones::PaceRange> pace;
};

// Returns an empty fingerprint when the immutable zone snapshot is missing or
// weight is invalid. A present snapshot with absent zone domains is a valid
// legacy input and produces a fingerprint that records those absences.
QByteArray rideRefreshCacheAnalysisFingerprint(
    const RideRefreshZones *zones,
    const RideRefreshCacheSettings &settings,
    const QDate &date,
    const QString &sport,
    bool isSwim,
    double weight);

// Copies the selected ranges so downstream cache computation does not retain
// pointers into another object. valid distinguishes a missing generation
// snapshot from valid, explicitly absent domains or date ranges.
RideRefreshDistributionZones rideRefreshDistributionZones(
    const RideRefreshZones *zones,
    const QDate &date,
    const QString &sport,
    bool isSwim);

#endif // GC_RIDEREFRESHCACHEINPUTS_H
