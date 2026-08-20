/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameRoadProjection.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double Pi = 3.14159265358979323846;

bool validConfig(const WorkoutGameRoadProjectionConfig &config)
{
    return std::isfinite(config.viewportWidth)
            && std::isfinite(config.viewportHeight)
            && std::isfinite(config.horizonRatio)
            && std::isfinite(config.cameraHeightMeters)
            && std::isfinite(config.fieldOfViewDegrees)
            && std::isfinite(config.nearDistanceMeters)
            && std::isfinite(config.visibleDistanceMeters)
            && config.viewportWidth > 0.0
            && config.viewportHeight > 0.0
            && config.horizonRatio > 0.05
            && config.horizonRatio < 0.8
            && config.cameraHeightMeters > 0.0
            && config.fieldOfViewDegrees > 20.0
            && config.fieldOfViewDegrees < 140.0
            && config.nearDistanceMeters > 0.0
            && config.visibleDistanceMeters > config.nearDistanceMeters
            && config.sliceCount >= 8
            && config.sliceCount <= 512;
}

}

WorkoutGameRoadProjectionFrame WorkoutGameRoadProjection::project(
        const WorkoutGameRoadCourse &course,
        double riderDistanceMeters,
        const WorkoutGameRoadProjectionConfig &config)
{
    WorkoutGameRoadProjectionFrame result;
    if (!course.ready || !std::isfinite(riderDistanceMeters)
            || !validConfig(config)) {
        return result;
    }
    const WorkoutGameRoadSample rider = WorkoutGameRoadCourseBuilder::sample(
            course, riderDistanceMeters);
    if (!rider.ready) return result;

    const double focalLength = config.viewportWidth * 0.5
            / std::tan(config.fieldOfViewDegrees * Pi / 360.0);
    const double horizonY = config.viewportHeight * config.horizonRatio;
    const double cameraX = rider.center.xMeters;
    const double cameraZ = rider.center.zMeters;
    const double cameraElevation = rider.center.elevationMeters
            + config.cameraHeightMeters;
    const double cameraHeading = rider.center.headingRadians;
    result.slices.reserve(std::size_t(config.sliceCount + 1));

    for (int index = config.sliceCount; index >= 0; --index) {
        const double amount = double(index) / double(config.sliceCount);
        const double ahead = config.nearDistanceMeters
                + config.visibleDistanceMeters * amount * amount;
        const WorkoutGameRoadSample sample =
                WorkoutGameRoadCourseBuilder::sample(
                    course, rider.distanceMeters + ahead);
        if (!sample.ready) continue;

        const double worldX = sample.center.xMeters - cameraX;
        const double worldZ = sample.center.zMeters - cameraZ;
        const double localX = worldX * std::cos(cameraHeading)
                - worldZ * std::sin(cameraHeading);
        const double localZ = worldX * std::sin(cameraHeading)
                + worldZ * std::cos(cameraHeading);
        if (localZ <= 0.2) continue;

        WorkoutGameRoadProjectedSlice slice;
        slice.worldDistanceMeters = sample.distanceMeters;
        slice.depthMeters = localZ;
        slice.centerX = config.viewportWidth * 0.5
                + focalLength * localX / localZ;
        slice.centerY = horizonY - focalLength
                * (sample.center.elevationMeters - cameraElevation) / localZ;
        slice.halfWidthPixels = std::clamp(
                focalLength * sample.center.halfWidthMeters / localZ,
                0.25,
                config.viewportWidth);
        slice.halfWidthMeters = sample.center.halfWidthMeters;
        slice.pixelsPerMeter = focalLength / localZ;
        slice.pieceIndex = sample.pieceIndex;
        slice.terrain = sample.terrain;
        result.slices.push_back(slice);
    }
    result.ready = result.slices.size() >= 2;
    result.riderScreenX = config.viewportWidth * 0.5;
    result.riderScreenY = config.viewportHeight * 0.82;
    return result;
}

WorkoutGameRoadProjectedPoint WorkoutGameRoadProjection::projectPoint(
        const WorkoutGameRoadProjectionFrame &frame,
        double worldDistanceMeters,
        double lateralMeters,
        double elevationMeters)
{
    WorkoutGameRoadProjectedPoint result;
    if (!frame.ready || frame.slices.size() < 2
            || !std::isfinite(worldDistanceMeters)
            || !std::isfinite(lateralMeters)
            || !std::isfinite(elevationMeters)
            || worldDistanceMeters > frame.slices.front().worldDistanceMeters
            || worldDistanceMeters < frame.slices.back().worldDistanceMeters) {
        return result;
    }
    for (std::size_t index = 1; index < frame.slices.size(); ++index) {
        const WorkoutGameRoadProjectedSlice &far = frame.slices[index - 1];
        const WorkoutGameRoadProjectedSlice &near = frame.slices[index];
        if (worldDistanceMeters > far.worldDistanceMeters
                || worldDistanceMeters < near.worldDistanceMeters) {
            continue;
        }
        const double span = far.worldDistanceMeters - near.worldDistanceMeters;
        const double amount = span > 1e-9
                ? (far.worldDistanceMeters - worldDistanceMeters) / span
                : 0.0;
        const auto interpolate = [amount](double from, double to) {
            return from + (to - from) * amount;
        };
        const double centerX = interpolate(far.centerX, near.centerX);
        const double centerY = interpolate(far.centerY, near.centerY);
        const double scale = interpolate(
                far.pixelsPerMeter, near.pixelsPerMeter);
        result.ready = true;
        result.x = centerX + lateralMeters * scale;
        result.y = centerY - elevationMeters * scale;
        result.depthMeters = interpolate(far.depthMeters, near.depthMeters);
        return result;
    }
    return result;
}
