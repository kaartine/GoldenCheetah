/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshRoutes.h"

#include <QObject>
#include <QThread>

#include <utility>

std::shared_ptr<const RideRefreshRoutes>
captureRideRefreshRoutesForOwner(
    const QObject *owner,
    QVector<RideRefreshRoutes::Segment> segments,
    quint16 fingerprint)
{
    if (!owner || QThread::currentThread() != owner->thread()) return {};
    return RideRefreshRoutes::create(std::move(segments), fingerprint);
}
