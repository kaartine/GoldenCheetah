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

#include <utility>

std::shared_ptr<const RideRefreshMeasures>
captureRideRefreshMeasures(const Athlete *athlete)
{
    if (!athlete || QThread::currentThread() != athlete->thread()
        || !athlete->measures) {
        return {};
    }

    QVector<RideRefreshMeasures::Group> groups;
    const QList<MeasuresGroup *> liveGroups = athlete->measures->getGroups();
    groups.reserve(liveGroups.size());
    for (MeasuresGroup *group : liveGroups)
        groups.append(captureRideRefreshMeasuresGroup(group));
    return RideRefreshMeasures::create(
        std::move(groups), Measure().getFingerprint());
}
