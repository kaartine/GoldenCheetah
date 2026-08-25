/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DTerrainProfile.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double Pi = 3.14159265358979323846;
constexpr double OuterTerrainMeters = 14.0;
constexpr double MidTerrainMeters = 6.0;
constexpr double ShoulderWidthMeters = 1.25;
constexpr double TrailSeamDropMeters = 0.02;

double ridgeHeight(
        double distanceMeters,
        std::uint32_t seed,
        double side)
{
    const double phase = double(seed % 8191u) / 8191.0 * 2.0 * Pi;
    const double broad = std::sin(
            distanceMeters * 0.037 + phase + side * 1.35);
    const double detail = std::sin(
            distanceMeters * 0.091 + phase * 0.63 - side * 0.82);
    return std::clamp(0.96 + broad * 0.30 + detail * 0.18, 0.72, 1.35);
}

WorkoutGame3DTerrainProfileVertex vertex(
        double lateralMeters,
        double elevationMeters,
        float red,
        float green,
        float blue)
{
    return {lateralMeters, elevationMeters, red, green, blue};
}

}

WorkoutGame3DTerrainProfileSnapshot WorkoutGame3DTerrainProfile::build(
        const WorkoutGameRoadSample &road,
        double distanceMeters,
        std::uint32_t seed)
{
    WorkoutGame3DTerrainProfileSnapshot result;
    const double halfWidth = road.center.halfWidthMeters;
    if (!road.ready || !std::isfinite(distanceMeters)
            || !std::isfinite(halfWidth) || halfWidth <= 0.0
            || halfWidth + ShoulderWidthMeters >= MidTerrainMeters
            || !std::isfinite(road.center.elevationMeters)
            || !std::isfinite(road.baseElevationMeters)) {
        return result;
    }

    const double seamElevation =
            road.center.elevationMeters - TrailSeamDropMeters;
    const double leftRidge = ridgeHeight(distanceMeters, seed, -1.0);
    const double rightRidge = ridgeHeight(distanceMeters, seed, 1.0);
    const double leftOuterElevation = road.baseElevationMeters + leftRidge;
    const double rightOuterElevation = road.baseElevationMeters + rightRidge;
    const double leftMidElevation = road.baseElevationMeters
            + leftRidge * 0.58;
    const double rightMidElevation = road.baseElevationMeters
            + rightRidge * 0.58;
    const double leftShoulderElevation = seamElevation
            + (leftMidElevation - seamElevation) * 0.34;
    const double rightShoulderElevation = seamElevation
            + (rightMidElevation - seamElevation) * 0.34;

    const float leftShade = float(std::clamp(
            (leftRidge - 0.72) / 0.63, 0.0, 1.0));
    const float rightShade = float(std::clamp(
            (rightRidge - 0.72) / 0.63, 0.0, 1.0));
    result.vertices = {{
        vertex(-OuterTerrainMeters, leftOuterElevation,
               0.15f, 0.29f + leftShade * 0.07f, 0.14f),
        vertex(-MidTerrainMeters, leftMidElevation,
               0.18f, 0.34f + leftShade * 0.06f, 0.17f),
        vertex(-(halfWidth + ShoulderWidthMeters), leftShoulderElevation,
               0.25f, 0.32f, 0.17f),
        vertex(-halfWidth, seamElevation, 0.36f, 0.29f, 0.15f),
        vertex(halfWidth, seamElevation, 0.36f, 0.29f, 0.15f),
        vertex(halfWidth + ShoulderWidthMeters, rightShoulderElevation,
               0.25f, 0.32f, 0.17f),
        vertex(MidTerrainMeters, rightMidElevation,
               0.18f, 0.34f + rightShade * 0.06f, 0.17f),
        vertex(OuterTerrainMeters, rightOuterElevation,
               0.15f, 0.29f + rightShade * 0.07f, 0.14f)
    }};
    result.ready = true;
    return result;
}

double WorkoutGame3DTerrainProfile::elevationAtLateral(
        const WorkoutGame3DTerrainProfileSnapshot &profile,
        double lateralMeters)
{
    if (!profile.ready || !std::isfinite(lateralMeters)) {
        return 0.0;
    }
    const auto &vertices = profile.vertices;
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        if (!std::isfinite(vertices[index].lateralMeters)
                || !std::isfinite(vertices[index].elevationMeters)
                || (index > 0 && vertices[index].lateralMeters
                    <= vertices[index - 1].lateralMeters)) {
            return 0.0;
        }
    }
    if (lateralMeters <= vertices.front().lateralMeters) {
        return vertices.front().elevationMeters;
    }
    if (lateralMeters >= vertices.back().lateralMeters) {
        return vertices.back().elevationMeters;
    }
    for (std::size_t index = 1; index < vertices.size(); ++index) {
        const auto &left = vertices[index - 1];
        const auto &right = vertices[index];
        if (lateralMeters <= right.lateralMeters) {
            const double span = right.lateralMeters - left.lateralMeters;
            const double ratio = std::clamp(
                    (lateralMeters - left.lateralMeters) / span,
                    0.0, 1.0);
            return left.elevationMeters
                    + (right.elevationMeters - left.elevationMeters) * ratio;
        }
    }
    return vertices.back().elevationMeters;
}
