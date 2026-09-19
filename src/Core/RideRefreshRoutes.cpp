/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshRoutes.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr double Pi = 3.14159265358979323846;

bool validStartPoint(const RideRefreshRoutes::RidePoint &point)
{
    return point.latitude != 0.0 && point.longitude != 0.0
        && std::ceil(point.latitude) != 180.0
        && std::ceil(point.longitude) != 180.0
        && std::ceil(point.latitude) != 540.0
        && std::ceil(point.longitude) != 540.0;
}

bool validLookAheadPoint(const RideRefreshRoutes::RidePoint &point)
{
    return point.latitude != 0.0 && point.longitude != 0.0
        && std::ceil(point.latitude) != 180.0
        && std::ceil(point.longitude) != 180.0;
}

} // namespace

std::shared_ptr<const RideRefreshRoutes> RideRefreshRoutes::create(
    QVector<Segment> segments,
    quint16 fingerprint)
{
    return create(
        std::move(segments), fingerprint, SearchParameters{});
}

std::shared_ptr<const RideRefreshRoutes> RideRefreshRoutes::create(
    QVector<Segment> segments,
    quint16 fingerprint,
    SearchParameters parameters)
{
    return std::shared_ptr<const RideRefreshRoutes>(
        new RideRefreshRoutes(
            std::move(segments), fingerprint, parameters));
}

RideRefreshRoutes::RideRefreshRoutes(
    QVector<Segment> segments,
    quint16 fingerprint,
    SearchParameters parameters)
    : segments_(std::move(segments))
    , fingerprint_(fingerprint)
    , parameters_(parameters)
{
}

QVector<RideRefreshRoutes::Match>
RideRefreshRoutes::search(const Ride &ride) const
{
    QVector<Match> matches;
    for (const Segment &segment : segments_) {
        if (ride.minimumLatitude
                    < segment.minimumLatitude
                        + parameters_.boundsToleranceDegrees
            && ride.maximumLatitude
                    > segment.maximumLatitude
                        - parameters_.boundsToleranceDegrees
            && ride.minimumLongitude
                    < segment.minimumLongitude
                        + parameters_.boundsToleranceDegrees
            && ride.maximumLongitude
                    > segment.maximumLongitude
                        - parameters_.boundsToleranceDegrees) {
            matches += searchSegment(segment, ride, parameters_);
        }
    }
    return matches;
}

QVector<RideRefreshRoutes::Match> RideRefreshRoutes::searchSegment(
    const Segment &segment,
    const Ride &ride,
    const SearchParameters &parameters)
{
    QVector<Match> matches;
    double precision = -1.0;
    int found = 0;
    int diverge = 0;
    int lastPoint = -1;
    double start = -1.0;
    double stop = -1.0;

    for (int routeIndex = 0; routeIndex < segment.points.size();
         ++routeIndex) {
        const Point routePoint = segment.points.at(routeIndex);
        bool resetRoute = false;
        bool present = false;
        const RidePoint *point = nullptr;

        for (int rideIndex = lastPoint + 1;
             rideIndex < ride.points.size(); ++rideIndex) {
            point = &ride.points.at(rideIndex);
            double minimumDistance = -1.0;

            if (validStartPoint(*point)) {
                if (start == -1.0) {
                    diverge = 0;
                    const double candidate = distanceKilometres(
                        routePoint.latitude,
                        routePoint.longitude,
                        point->latitude,
                        point->longitude);
                    minimumDistance = candidate;
                    if (precision == -1.0 || candidate < precision)
                        precision = candidate;
                    if (candidate > 1.0) {
                        rideIndex += parameters.farPointSkip;
                    } else if (candidate < parameters.minimumPrecisionKilometres) {
                        start = 0.0;
                    }
                }

                if (start != -1.0) {
                    int end = rideIndex + parameters.lookAhead;
                    for (int lookAhead = rideIndex;
                         lookAhead < ride.points.size() && lookAhead < end;
                         ++lookAhead) {
                        const RidePoint &next = ride.points.at(lookAhead);
                        if (!validLookAheadPoint(next)) continue;
                        const double nextDistance = distanceKilometres(
                            routePoint.latitude,
                            routePoint.longitude,
                            next.latitude,
                            next.longitude);
                        if (minimumDistance == -1.0
                            || nextDistance < minimumDistance) {
                            point = &next;
                            rideIndex = lookAhead;
                            minimumDistance = nextDistance;
                        }
                        if (nextDistance
                            <= parameters.minimumPrecisionKilometres) {
                            if (nextDistance < minimumDistance * 1.2)
                                end = lookAhead + parameters.lookAhead;
                            else
                                lookAhead = end;
                        }
                        if (nextDistance
                            <= parameters.maximumPrecisionKilometres) {
                            lookAhead = end;
                        }
                    }

                    if (minimumDistance
                        <= parameters.minimumPrecisionKilometres) {
                        present = true;
                        lastPoint = rideIndex;
                        if (start == 0.0) {
                            start = point->seconds;
                            precision = 0.0;
                        }
                        if (minimumDistance > precision)
                            precision = minimumDistance;
                        break;
                    }

                    ++diverge;
                    if (diverge > parameters.maximumDivergence) {
                        resetRoute = true;
                        break;
                    }
                }
            }
        }

        if (!present && !resetRoute) break;
        if (point) stop = point->seconds;

        if (routeIndex == segment.points.size() - 1) {
            matches.append({
                segment.id,
                segment.name,
                start,
                stop,
                distanceAt(ride, start),
                distanceAt(ride, stop),
                ++found
            });
            resetRoute = true;
        }

        if (resetRoute) {
            start = -1.0;
            routeIndex = -1;
            lastPoint += parameters.restartSkip;
        }
    }
    return matches;
}

double RideRefreshRoutes::distanceKilometres(
    double latitude1,
    double longitude1,
    double latitude2,
    double longitude2)
{
    const double theta = longitude1 - longitude2;
    if (theta == 0.0 && latitude1 == latitude2) return 0.0;
    const auto radians = [](double degrees) {
        return degrees * Pi / 180.0;
    };
    const double distance =
        std::sin(radians(latitude1)) * std::sin(radians(latitude2))
        + std::cos(radians(latitude1)) * std::cos(radians(latitude2))
            * std::cos(radians(theta));
    return std::acos(distance) * 6371.0;
}

double RideRefreshRoutes::distanceAt(
    const Ride &ride, double seconds)
{
    if (ride.points.isEmpty()) return 0.0;
    if (seconds < ride.points.constFirst().seconds)
        return ride.points.constFirst().kilometres;
    if (seconds > ride.points.constLast().seconds)
        return ride.points.constLast().kilometres;
    const auto found = std::lower_bound(
        ride.points.cbegin(), ride.points.cend(), seconds,
        [](const RidePoint &point, double value) {
            return point.seconds < value;
        });
    return found->kilometres;
}
