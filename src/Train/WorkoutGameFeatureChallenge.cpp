/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameFeatureChallenge.h"

#include <algorithm>
#include <cmath>

namespace {

double finiteNonNegative(double value)
{
    return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

double requirementRatio(double actual, double minimum)
{
    if (minimum <= 0.0) return 1.0;
    return std::clamp(finiteNonNegative(actual) / minimum, 0.0, 1.0);
}

WorkoutGameFeatureChallengeProfile baseProfile(
        WorkoutGameTerrainKind terrain)
{
    using Cue = WorkoutGameChallengeCue;
    switch (terrain) {
    case WorkoutGameTerrainKind::Roots:
        return {true, Cue::CarrySpeed, 0.35, 0.85,
                1.0, 0.0, 0.0, 0.0, 0.0, 0};
    case WorkoutGameTerrainKind::Rollers:
        return {true, Cue::CarrySpeed, 0.30, 0.82,
                1.0, 0.0, 0.0, 0.0, 0.0, 0};
    case WorkoutGameTerrainKind::Climb:
        return {true, Cue::Climb, 0.0, 0.95,
                1.0, 0.0, 0.0, 0.0, 0.0, 0};
    case WorkoutGameTerrainKind::RockGarden:
        return {true, Cue::CarrySpeed, 0.35, 0.85,
                1.0, 0.0, 0.0, 0.0, 0.0, 0};
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver:
        return {true, Cue::Jump, 0.50, 0.58,
                1.0, 0.0, 0.0, 0.0, 0.0, 0};
    case WorkoutGameTerrainKind::Drop:
        return {true, Cue::CarrySpeed, 0.43, 0.55,
                1.0, 0.0, 0.0, 0.0, 0.0, 0};
    case WorkoutGameTerrainKind::Skinny:
        return {true, Cue::HoldLine, 0.20, 0.85,
                1.0, 0.0, 0.0, 0.0, 0.0, 0};
    case WorkoutGameTerrainKind::Berm:
        return {};
    case WorkoutGameTerrainKind::Tabletop:
        return {true, Cue::Jump, 0.62, 0.72,
                1.0, 0.0, 0.0, 0.0, 0.0, 0};
    case WorkoutGameTerrainKind::RockSlab:
        return {true, Cue::CarrySpeed, 0.35, 0.88,
                1.0, 0.0, 0.0, 0.0, 0.0, 0};
    case WorkoutGameTerrainKind::SmoothTrail:
        return {};
    }
    return {};
}

}

WorkoutGameFeatureChallengeProfile WorkoutGameFeatureChallenge::profile(
        const WorkoutGameSection &section)
{
    if (section.challengeCount <= 0) return {};

    WorkoutGameFeatureChallengeProfile result = baseProfile(section.terrain);
    if (!result.enabled) return result;

    const double difficulty = std::clamp(
            std::isfinite(section.difficulty) ? section.difficulty : 0.0,
            0.0,
            1.0);
    const int challenges = std::clamp(section.challengeCount, 1, 10);
    result.bonusPoints = std::uint64_t(std::llround(
            250.0 + 100.0 * double(challenges) + 150.0 * difficulty));
    return result;
}

WorkoutGameFeatureChallengeAssessment WorkoutGameFeatureChallenge::assess(
        const WorkoutGameFeatureChallengeProfile &profile,
        const WorkoutGameFeatureChallengeMetrics &metrics)
{
    WorkoutGameFeatureChallengeAssessment result;
    if (!profile.enabled) return result;

    result.effortReadiness = requirementRatio(
            metrics.averageEffortRatio, profile.minimumEffortRatio);
    result.cadenceReadiness = requirementRatio(
            metrics.averageCadenceRpm, profile.minimumCadenceRpm);
    result.speedReadiness = requirementRatio(
            metrics.averageSpeedKph, profile.minimumSpeedKph);
    result.adherenceReadiness = requirementRatio(
            metrics.averageAdherence, profile.minimumAdherence);
    if (profile.maximumSpeedKph > 0.0) {
        const double speed = finiteNonNegative(metrics.averageSpeedKph);
        result.speedReadiness = std::min(
                result.speedReadiness,
                speed > 0.0
                        ? std::clamp(profile.maximumSpeedKph / speed, 0.0, 1.0)
                        : 0.0);
    }

    result.readiness = std::min({
        result.effortReadiness,
        result.cadenceReadiness,
        result.speedReadiness,
        result.adherenceReadiness
    });
    result.completed = result.readiness >= 1.0 - 1e-9;
    return result;
}
