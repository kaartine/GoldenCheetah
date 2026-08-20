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
                0.78, 60.0, 7.0, 0.0, 0.75, 0};
    case WorkoutGameTerrainKind::Rollers:
        return {true, Cue::CarrySpeed, 0.30, 0.82,
                0.78, 65.0, 10.0, 0.0, 0.72, 0};
    case WorkoutGameTerrainKind::Climb:
        return {true, Cue::Climb, 0.0, 0.95,
                0.80, 50.0, 0.0, 0.0, 0.75, 0};
    case WorkoutGameTerrainKind::RockGarden:
        return {true, Cue::CarrySpeed, 0.35, 0.85,
                0.80, 60.0, 8.0, 0.0, 0.72, 0};
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver:
        return {true, Cue::Jump, 0.45, 0.65,
                0.90, 65.0, 12.0, 0.0, 0.70, 0};
    case WorkoutGameTerrainKind::Drop:
        return {true, Cue::CarrySpeed, 0.45, 0.68,
                0.0, 0.0, 12.0, 0.0, 0.0, 0};
    case WorkoutGameTerrainKind::Skinny:
        return {true, Cue::HoldLine, 0.20, 0.85,
                0.75, 60.0, 6.0, 22.0, 0.82, 0};
    case WorkoutGameTerrainKind::Berm:
        return {true, Cue::CarrySpeed, 0.40, 0.78,
                0.0, 0.0, 14.0, 0.0, 0.0, 0};
    case WorkoutGameTerrainKind::Tabletop:
        return {true, Cue::Jump, 0.45, 0.70,
                0.92, 70.0, 16.0, 0.0, 0.72, 0};
    case WorkoutGameTerrainKind::RockSlab:
        return {true, Cue::CarrySpeed, 0.35, 0.88,
                0.82, 55.0, 7.0, 0.0, 0.75, 0};
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
    if (result.minimumEffortRatio > 0.0) {
        result.minimumEffortRatio = std::min(
                1.0, result.minimumEffortRatio + 0.05 * difficulty);
    }
    if (result.minimumSpeedKph > 0.0) {
        result.minimumSpeedKph *= 0.9 + 0.2 * difficulty;
    }
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

    double readiness = 1.0;
    readiness = std::min(readiness, requirementRatio(
            metrics.averageEffortRatio, profile.minimumEffortRatio));
    readiness = std::min(readiness, requirementRatio(
            metrics.averageCadenceRpm, profile.minimumCadenceRpm));
    readiness = std::min(readiness, requirementRatio(
            metrics.averageSpeedKph, profile.minimumSpeedKph));
    readiness = std::min(readiness, requirementRatio(
            metrics.averageAdherence, profile.minimumAdherence));
    if (profile.maximumSpeedKph > 0.0) {
        const double speed = finiteNonNegative(metrics.averageSpeedKph);
        readiness = std::min(
                readiness,
                speed > 0.0
                        ? std::clamp(profile.maximumSpeedKph / speed, 0.0, 1.0)
                        : 0.0);
    }

    result.readiness = std::clamp(readiness, 0.0, 1.0);
    result.completed = result.readiness >= 1.0 - 1e-9;
    return result;
}
