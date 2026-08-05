/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "MapRoutePointIndex.h"

#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double SearchRadiusDegrees = 0.001;

bool validCoordinate(double latitude, double longitude)
{
    return std::isfinite(latitude)
        && std::isfinite(longitude)
        && latitude >= -90.0
        && latitude <= 90.0
        && longitude >= -180.0
        && longitude <= 180.0;
}

} // namespace

void MapRoutePointIndex::clear()
{
    entries_.clear();
    finalized_ = true;
}

void MapRoutePointIndex::reserve(qsizetype count)
{
    if (count > 0)
        entries_.reserve(count);
}

bool MapRoutePointIndex::append(double latitude,
                                double longitude,
                                qsizetype sourceIndex)
{
    if (!validCoordinate(latitude, longitude)
        || sourceIndex < 0) {
        return false;
    }
    entries_.append({latitude, longitude, sourceIndex});
    finalized_ = false;
    return true;
}

void MapRoutePointIndex::finalize()
{
    std::sort(
        entries_.begin(), entries_.end(),
        [](const Entry &left, const Entry &right) {
            return left.latitude == right.latitude
                ? left.sourceIndex < right.sourceIndex
                : left.latitude < right.latitude;
        });
    finalized_ = true;
}

qsizetype MapRoutePointIndex::nearest(
    double latitude,
    double longitude,
    qsizetype *examined) const
{
    if (examined)
        *examined = 0;
    if (!finalized_
        || !validCoordinate(latitude, longitude)
        || entries_.isEmpty()) {
        return -1;
    }

    const double minimumLatitude =
        latitude - SearchRadiusDegrees;
    const double maximumLatitude =
        latitude + SearchRadiusDegrees;
    auto current = std::lower_bound(
        entries_.cbegin(), entries_.cend(), minimumLatitude,
        [](const Entry &entry, double value) {
            return entry.latitude < value;
        });

    const double longitudeScale =
        std::cos(qDegreesToRadians(latitude));
    double bestDistance =
        std::numeric_limits<double>::max();
    qsizetype bestIndex = -1;
    for (; current != entries_.cend()
           && current->latitude < maximumLatitude;
         ++current) {
        if (examined)
            ++*examined;
        const double deltaLatitude =
            std::fabs(current->latitude - latitude);
        const double deltaLongitude =
            std::fabs(current->longitude - longitude);
        if (deltaLatitude >= SearchRadiusDegrees
            || deltaLongitude >= SearchRadiusDegrees) {
            continue;
        }
        const double scaledLongitude =
            deltaLongitude * longitudeScale;
        const double distance = std::sqrt(
            scaledLongitude * scaledLongitude
            + deltaLatitude * deltaLatitude);
        if (distance < bestDistance
            || (distance == bestDistance
                && (bestIndex < 0
                    || current->sourceIndex < bestIndex))) {
            bestDistance = distance;
            bestIndex = current->sourceIndex;
        }
    }
    return bestIndex;
}
