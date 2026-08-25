/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DFeatureAsset.h"

#include "WorkoutGameFeatureGeometry.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double Pi = 3.14159265358979323846;

struct AssetSpec
{
    bool ready = false;
    double deadZoneMeters = 0.0;
    double coreLengthMeters = 0.0;
    double heightMeters = 0.0;
};

AssetSpec specFor(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::BunnyHop:
        return {true, 1.68, 0.14, 0.20};
    case WorkoutGameTerrainKind::LogOver:
        return {true, 0.75, 0.54, 0.54};
    case WorkoutGameTerrainKind::Tabletop:
        return {true, 0.75, 4.84, 0.446};
    default:
        return {};
    }
}

}

WorkoutGame3DFeatureAssetSnapshot WorkoutGame3DFeatureAsset::place(
        const WorkoutGameRoadCourse &course,
        const WorkoutGameRoadPiece &piece)
{
    WorkoutGame3DFeatureAssetSnapshot result;
    const AssetSpec spec = specFor(piece.terrain);
    const WorkoutGameFeatureGeometryProfile profile =
            WorkoutGameFeatureGeometry::profile(
                piece.terrain, piece.difficulty);
    if (!course.ready || !piece.challenge.enabled || !spec.ready
            || !profile.ready || profile.heightMeters <= 0.0
            || spec.coreLengthMeters <= 0.0 || spec.heightMeters <= 0.0) {
        return result;
    }
    const double coreLength = profile.endMeters - profile.startMeters;
    const double scaleZ = coreLength / spec.coreLengthMeters;
    const double scaleY = profile.heightMeters / spec.heightMeters;
    if (!std::isfinite(scaleZ) || !std::isfinite(scaleY)
            || scaleZ <= 0.0 || scaleY <= 0.0) {
        return result;
    }
    const double assetStartDistance = std::clamp(
            piece.challenge.obstacleDistanceMeters + profile.startMeters
                - spec.deadZoneMeters * scaleZ,
            0.0, course.totalLengthMeters);
    const WorkoutGameRoadSample sample =
            WorkoutGameRoadCourseBuilder::sample(course, assetStartDistance);
    if (!sample.ready) return result;

    result.ready = true;
    result.terrain = piece.terrain;
    result.xMeters = sample.center.xMeters;
    result.yMeters = sample.visualGroundElevationMeters();
    result.zMeters = sample.center.zMeters;
    result.yawDegrees = sample.center.headingRadians * 180.0 / Pi;
    result.pitchDegrees = -std::atan(sample.baseGradePercent / 100.0)
            * 180.0 / Pi;
    result.scaleY = scaleY;
    result.scaleZ = scaleZ;
    return result;
}
