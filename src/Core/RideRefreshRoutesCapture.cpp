/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshRoutes.h"

#include "Route.h"

#include <QThread>

#include <utility>

std::shared_ptr<const RideRefreshRoutes>
captureRideRefreshRoutes(const Routes *source)
{
    if (!source || QThread::currentThread() != source->thread()) return {};

    QVector<RideRefreshRoutes::Segment> segments;
    segments.reserve(source->routes.size());
    for (const RouteSegment &live : source->routes) {
        RideRefreshRoutes::Segment segment;
        segment.id = live._id;
        segment.name = live.name;
        segment.minimumLatitude = live.minLat;
        segment.maximumLatitude = live.maxLat;
        segment.minimumLongitude = live.minLon;
        segment.maximumLongitude = live.maxLon;
        segment.points.reserve(live.points.size());
        for (const RoutePoint &point : live.points)
            segment.points.append({point.lon, point.lat});
        segments.append(std::move(segment));
    }
    return captureRideRefreshRoutesForOwner(
        source, std::move(segments), source->getFingerprint());
}
