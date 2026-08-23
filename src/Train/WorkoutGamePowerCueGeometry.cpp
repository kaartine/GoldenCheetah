/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGamePowerCueGeometry.h"

#include <algorithm>
#include <cmath>

namespace {

double leadDistance(WorkoutGameChallengeCue cue)
{
    switch (cue) {
    case WorkoutGameChallengeCue::Jump: return 5.5;
    case WorkoutGameChallengeCue::CarrySpeed: return 7.0;
    case WorkoutGameChallengeCue::HoldLine: return 6.0;
    case WorkoutGameChallengeCue::Climb:
    case WorkoutGameChallengeCue::None:
        return 0.0;
    }
    return 0.0;
}

}

std::vector<WorkoutGamePowerCueBand> WorkoutGamePowerCueGeometry::build(
        const WorkoutGameFeatureRuntimeSnapshot &feature,
        WorkoutGameChallengeCue cue)
{
    std::vector<WorkoutGamePowerCueBand> result;
    const double lead = leadDistance(cue);
    if (!feature.ready || lead <= 0.0
            || feature.phase == WorkoutGameFeaturePhase::Action
            || feature.phase == WorkoutGameFeaturePhase::Recovery
            || feature.decisionDistanceMeters
                <= feature.prepareDistanceMeters
            || !std::isfinite(feature.visualDistanceMeters)
            || !std::isfinite(feature.prepareDistanceMeters)
            || !std::isfinite(feature.decisionDistanceMeters)) {
        return result;
    }

    constexpr double StripeLengthMeters = 0.48;
    constexpr double StripeSpacingMeters = 2.0;
    constexpr double HalfWidthRatio = 0.84;
    const double start = std::max(
            feature.prepareDistanceMeters,
            feature.decisionDistanceMeters - lead);
    for (double distance = start;
         distance < feature.decisionDistanceMeters;
         distance += StripeSpacingMeters) {
        result.push_back({
            distance,
            std::min(distance + StripeLengthMeters,
                     feature.decisionDistanceMeters),
            HalfWidthRatio,
            false
        });
    }
    result.push_back({
        feature.decisionDistanceMeters - 0.32,
        feature.decisionDistanceMeters + 0.32,
        0.90,
        true
    });
    return result;
}
