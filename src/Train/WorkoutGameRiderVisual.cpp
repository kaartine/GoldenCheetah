/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameRiderVisual.h"

#include <algorithm>
#include <cmath>

WorkoutGameRiderVisualPose WorkoutGameRiderVisual::pose(
        const WorkoutGameWorldSnapshot &world,
        const WorkoutGameFeatureRuntimeSnapshot &feature,
        double riderHeightPixels)
{
    WorkoutGameRiderVisualPose result;
    const double featureAir = !world.ready && feature.ready
            && feature.route != WorkoutGameRoute::SafeBypass
            && feature.outcome == WorkoutGameFeatureOutcome::Completed
            ? feature.verticalOffsetMeters : 0.0;
    const double authoritativeAir = world.ready
            ? world.rider.airHeightMeters() : featureAir;
    result.airHeightMeters = std::max(
            0.0,
            std::isfinite(authoritativeAir) ? authoritativeAir : 0.0);
    result.airborne = result.airHeightMeters > 0.05;

    // The on-screen rider establishes the near-field perspective scale.
    // Tying lift to that size keeps the same jump readable at every viewport.
    constexpr double RiderVisualHeightMeters = 1.85;
    const double pixelsPerMeter = std::clamp(
            (std::isfinite(riderHeightPixels) ? riderHeightPixels : 0.0)
                / RiderVisualHeightMeters,
            48.0, 150.0);
    result.liftPixels = result.airHeightMeters * pixelsPerMeter;

    const double flight = std::clamp(
            result.airHeightMeters / 1.35, 0.0, 1.0);
    result.shadowScale = std::clamp(
            1.0 - 0.38 * flight, 0.55, 1.0);
    result.shadowOpacity = std::clamp(
            0.44 - 0.20 * flight, 0.20, 0.44);
    result.riderWidthScale = 1.0 + 0.035 * flight;
    result.riderHeightScale = 1.0 - 0.065 * flight;

    // In the chase view pitch changes foreshortening, not screen-space roll.
    result.screenRollDegrees = world.ready
            && std::isfinite(world.rider.rollDegrees)
            ? -world.rider.rollDegrees : 0.0;
    return result;
}
