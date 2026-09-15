/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DCameraComfort.h"

#include <algorithm>
#include <cmath>

double WorkoutGame3DCameraComfort::supportElevationMeters(
        const WorkoutGameRoadSample &sample)
{
    if (!sample.ready) return 0.0;
    double elevation = sample.visualGroundElevationMeters();
    if (sample.terrain == WorkoutGameTerrainKind::Roots) {
        elevation -= std::max(0.0, sample.surfaceOffsetMeters);
    }
    return std::isfinite(elevation) ? elevation : 0.0;
}

void WorkoutGame3DCameraComfort::reset()
{
    initialized = false;
    lastWorkoutTimeMs = 0;
    currentOffsetMeters = 0.0;
}

double WorkoutGame3DCameraComfort::update(
        const WorkoutGame3DCameraComfortInput &input)
{
    const std::int64_t now = std::max<std::int64_t>(0, input.workoutTimeMs);
    if (!initialized || now < lastWorkoutTimeMs) {
        initialized = true;
        lastWorkoutTimeMs = now;
        currentOffsetMeters = 0.0;
        return currentOffsetMeters;
    }

    const double elapsedSeconds = std::clamp(
            double(now - lastWorkoutTimeMs) / 1000.0, 0.0, 0.10);
    lastWorkoutTimeMs = now;
    double targetOffsetMeters = 0.0;
    if (input.mainLine
            && input.terrain == WorkoutGameTerrainKind::Roots
            && std::isfinite(input.surfaceOffsetMeters)) {
        targetOffsetMeters = std::clamp(
                input.surfaceOffsetMeters * 0.16,
                0.0, MaximumRootBumpMeters);
    }

    constexpr double TimeConstantSeconds = 0.060;
    const double blend = 1.0 - std::exp(
            -elapsedSeconds / TimeConstantSeconds);
    const double maximumStep = MaximumVerticalSpeedMetersPerSecond
            * elapsedSeconds;
    currentOffsetMeters += std::clamp(
            (targetOffsetMeters - currentOffsetMeters) * blend,
            -maximumStep, maximumStep);
    currentOffsetMeters = std::clamp(
            currentOffsetMeters, 0.0, MaximumRootBumpMeters);
    return currentOffsetMeters;
}
