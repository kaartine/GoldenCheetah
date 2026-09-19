/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshZones.h"

#include "Athlete.h"

#include <QThread>

std::shared_ptr<const RideRefreshZones>
captureRideRefreshZones(
    const Athlete *athlete,
    const QHash<QString, QVariant> &globalSettings,
    const QHash<QString, QVariant> &athleteSettings,
    bool useMetricUnits)
{
    if (!athlete || QThread::currentThread() != athlete->thread()) return {};
    return captureRideRefreshZonesForOwner(
        athlete,
        athlete->zones_,
        athlete->hrzones_,
        athlete->paceZones(false),
        athlete->paceZones(true),
        globalSettings,
        athleteSettings,
        useMetricUnits);
}
