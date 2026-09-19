/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshMeasures.h"

#include <QObject>
#include <QThread>

#include <utility>

std::shared_ptr<const RideRefreshMeasures>
captureRideRefreshMeasuresForOwner(
    const QObject *owner,
    const QList<MeasuresGroup *> &liveGroups,
    quint16 missingObservationFingerprint)
{
    if (!owner || QThread::currentThread() != owner->thread()) return {};

    QVector<RideRefreshMeasures::Group> groups;
    groups.reserve(liveGroups.size());
    for (MeasuresGroup *group : liveGroups)
        groups.append(captureRideRefreshMeasuresGroup(group));
    return RideRefreshMeasures::create(
        std::move(groups), missingObservationFingerprint);
}
