/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameRoadProjection.h"

#include "WorkoutGameFeatureGeometry.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr double Pi = 3.14159265358979323846;

bool validConfig(const WorkoutGameRoadProjectionConfig &config)
{
    return std::isfinite(config.viewportWidth)
            && std::isfinite(config.viewportHeight)
            && std::isfinite(config.horizonRatio)
            && std::isfinite(config.cameraHeightMeters)
            && (std::isfinite(config.cameraElevationMeters)
                || std::isnan(config.cameraElevationMeters))
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
    const double cameraElevation = std::isfinite(config.cameraElevationMeters)
            ? config.cameraElevationMeters
            : rider.center.elevationMeters + config.cameraHeightMeters;
    const double cameraHeading = rider.center.headingRadians;
    std::vector<double> sampleDistances;
    sampleDistances.reserve(std::size_t(config.sliceCount + 1)
            + course.pieces.size() * 5u);
    for (int index = config.sliceCount; index >= 0; --index) {
        const double amount = double(index) / double(config.sliceCount);
        const double ahead = config.nearDistanceMeters
                + config.visibleDistanceMeters * amount * amount;
        sampleDistances.push_back(rider.distanceMeters + ahead);
    }
    const double nearest = rider.distanceMeters + config.nearDistanceMeters;
    const double farthest = nearest + config.visibleDistanceMeters;
    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (!piece.challenge.enabled) continue;
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece.terrain, piece.difficulty);
        if (!profile.ready) continue;
        const double obstacle = piece.challenge.obstacleDistanceMeters;
        const double keyDistances[] = {
            obstacle + profile.startMeters,
            obstacle + profile.plateauStartMeters,
            obstacle + profile.plateauEndMeters,
            obstacle + profile.endMeters
        };
        for (double distance : keyDistances) {
            if (distance >= nearest && distance <= farthest) {
                sampleDistances.push_back(distance);
            }
        }
    }
    std::sort(sampleDistances.begin(), sampleDistances.end(), std::greater<>());
    sampleDistances.erase(std::unique(
            sampleDistances.begin(), sampleDistances.end(),
            [](double left, double right) {
                return std::abs(left - right) < 1e-7;
            }), sampleDistances.end());
    result.slices.reserve(sampleDistances.size());

    for (double sampleDistance : sampleDistances) {
        const WorkoutGameRoadSample sample =
                WorkoutGameRoadCourseBuilder::sample(
                    course, sampleDistance);
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
        slice.surfaceElevationMeters = sample.center.elevationMeters
                + sample.surfaceOffsetMeters;
        slice.centerY = horizonY - focalLength
                * (slice.surfaceElevationMeters - cameraElevation) / localZ;
        slice.halfWidthPixels = std::clamp(
                focalLength * sample.center.halfWidthMeters / localZ,
                0.25,
                config.viewportWidth);
        slice.halfWidthMeters = sample.center.halfWidthMeters;
        slice.pixelsPerMeter = focalLength / localZ;
        slice.surfaceOffsetMeters = sample.surfaceOffsetMeters;
        slice.gradePercent = sample.center.gradePercent;
        slice.pieceIndex = sample.pieceIndex;
        slice.terrain = sample.terrain;
        result.slices.push_back(slice);
    }
    double occlusionY = config.viewportHeight;
    for (auto slice = result.slices.rbegin();
         slice != result.slices.rend(); ++slice) {
        slice->occlusionY = occlusionY;
        occlusionY = std::min(occlusionY, slice->centerY);
    }
    result.ready = result.slices.size() >= 2;
    result.riderScreenX = config.viewportWidth * 0.5;
    result.riderScreenY = config.viewportHeight * 0.82;
    result.renderedGradePercent = rider.center.gradePercent;
    if (result.ready) {
        result.visibleElevationChangeMeters =
                result.slices.front().surfaceElevationMeters
                - (rider.center.elevationMeters
                   + rider.surfaceOffsetMeters);
    }
    return result;
}

WorkoutGameRoadProjectedPoint WorkoutGameRoadProjection::projectPoint(
        const WorkoutGameRoadProjectionFrame &frame,
        double worldDistanceMeters,
        double lateralMeters,
        double elevationMeters,
        bool relativeToBaseSurface)
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
        const double surfaceOffset = interpolate(
                far.surfaceOffsetMeters, near.surfaceOffsetMeters);
        const double relativeElevation = relativeToBaseSurface
                ? elevationMeters - surfaceOffset : elevationMeters;
        result.ready = true;
        result.x = centerX + lateralMeters * scale;
        result.y = centerY - relativeElevation * scale;
        result.depthMeters = interpolate(far.depthMeters, near.depthMeters);
        result.occlusionY = interpolate(far.occlusionY, near.occlusionY);
        result.visible = result.y <= result.occlusionY + 1e-6;
        return result;
    }
    return result;
}
