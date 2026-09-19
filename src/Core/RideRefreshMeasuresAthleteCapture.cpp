/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshMeasures.h"

#include "Athlete.h"
#include "Measures.h"

#include <QThread>

std::shared_ptr<const RideRefreshMeasures>
captureRideRefreshMeasures(const Athlete *athlete)
{
    if (!athlete || QThread::currentThread() != athlete->thread()
        || !athlete->measures) {
        return {};
    }

    return captureRideRefreshMeasuresForOwner(
        athlete,
        athlete->measures->getGroups(),
        Measure().getFingerprint());
}
