/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameGapJumpGeometry.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double MinimumFlightSeconds = 0.25;
constexpr double MaximumAuthoredFlightSeconds = 1.40;
constexpr double HardMaximumFlightSeconds = 2.0;

bool finitePositive(double value)
{
    return std::isfinite(value) && value > 0.0;
}

bool orderedLaterals(
        const std::array<WorkoutGameGapJumpLineDefinition, 3> &lines,
        double minimumSeparation)
{
    const double firstStep = lines[1].lateralMeters - lines[0].lateralMeters;
    const double secondStep = lines[2].lateralMeters - lines[1].lateralMeters;
    if (!std::isfinite(firstStep) || !std::isfinite(secondStep)) return false;
    const bool sameDirection = (firstStep > 0.0 && secondStep > 0.0)
            || (firstStep < 0.0 && secondStep < 0.0);
    return sameDirection && std::abs(firstStep) > minimumSeparation
            && std::abs(secondStep) > minimumSeparation;
}

}

WorkoutGameGapJumpGeometryProfile
WorkoutGameGapJumpGeometry::canonicalProfile()
{
    return profile(0.5);
}

WorkoutGameGapJumpGeometryProfile WorkoutGameGapJumpGeometry::profile(
        double requestedDifficulty)
{
    if (!std::isfinite(requestedDifficulty)) return {};

    const double difficulty = std::clamp(requestedDifficulty, 0.0, 1.0);
    const double signedDifficulty = (difficulty - 0.5) * 2.0;
    const double gapScale = 1.0 + 0.15 * signedDifficulty;
    const double speedScale = 1.0 + 0.10 * signedDifficulty;
    const double heightScale = 1.0 + 0.20 * signedDifficulty;
    const double flightScale = std::sqrt(gapScale / speedScale);

    WorkoutGameGapJumpGeometryProfile result;
    result.ready = true;
    result.difficulty = difficulty;
    result.socketHalfWidthMeters = 0.68;
    result.prepareLeadMeters = 45.0;
    result.lockLeadMeters = 32.0;
    result.splitLengthMeters = 20.0;
    result.mergeLengthMeters = 18.0;
    result.bypassLateralMeters = -4.6;
    result.featureStartMeters = 0.0;
    result.featureEndMeters = 36.0;
    result.hysteresisMetersPerSecond = 0.35;
    result.maximumFlightSeconds = HardMaximumFlightSeconds;
    result.lines = {{
        {WorkoutGameGapJumpLine::Short, -2.3, 1.8 * gapScale,
         4.0 * speedScale,
         std::clamp(0.55 * flightScale, MinimumFlightSeconds,
                    MaximumAuthoredFlightSeconds),
         0.48 * heightScale, 0.14},
        {WorkoutGameGapJumpLine::Medium, 0.0, 3.2 * gapScale,
         5.2 * speedScale,
         std::clamp(0.78 * flightScale, MinimumFlightSeconds,
                    MaximumAuthoredFlightSeconds),
         0.62 * heightScale, 0.22},
        {WorkoutGameGapJumpLine::Long, 2.3, 4.7 * gapScale,
         6.6 * speedScale,
         std::clamp(1.05 * flightScale, MinimumFlightSeconds,
                    MaximumAuthoredFlightSeconds),
         0.78 * heightScale, 0.32}
    }};

    if (!validate(result)) return {};
    return result;
}

WorkoutGameGapJumpGeometryProfile WorkoutGameGapJumpGeometry::mirrored(
        const WorkoutGameGapJumpGeometryProfile &source)
{
    if (!validate(source)) return {};
    WorkoutGameGapJumpGeometryProfile result = source;
    result.bypassLateralMeters = -result.bypassLateralMeters;
    for (auto &definition : result.lines) {
        definition.lateralMeters = -definition.lateralMeters;
    }
    if (!validate(result)) return {};
    return result;
}

bool WorkoutGameGapJumpGeometry::validate(
        const WorkoutGameGapJumpGeometryProfile &profile)
{
    if (!profile.ready || !std::isfinite(profile.difficulty)
            || profile.difficulty < 0.0 || profile.difficulty > 1.0
            || !finitePositive(profile.socketHalfWidthMeters)
            || !finitePositive(profile.prepareLeadMeters)
            || !finitePositive(profile.lockLeadMeters)
            || profile.prepareLeadMeters <= profile.lockLeadMeters
            || !finitePositive(profile.splitLengthMeters)
            || !finitePositive(profile.mergeLengthMeters)
            || !std::isfinite(profile.bypassLateralMeters)
            || !std::isfinite(profile.featureStartMeters)
            || !std::isfinite(profile.featureEndMeters)
            || profile.featureEndMeters <= profile.featureStartMeters
            || !finitePositive(profile.hysteresisMetersPerSecond)
            || !finitePositive(profile.maximumFlightSeconds)
            || profile.maximumFlightSeconds > HardMaximumFlightSeconds) {
        return false;
    }

    constexpr std::array<WorkoutGameGapJumpLine, 3> ExpectedIds = {
        WorkoutGameGapJumpLine::Short,
        WorkoutGameGapJumpLine::Medium,
        WorkoutGameGapJumpLine::Long
    };
    for (std::size_t index = 0; index < profile.lines.size(); ++index) {
        const auto &definition = profile.lines[index];
        if (definition.id != ExpectedIds[index]
                || !std::isfinite(definition.lateralMeters)
                || !finitePositive(definition.gapLengthMeters)
                || !finitePositive(
                    definition.coldThresholdMetersPerSecond)
                || !finitePositive(definition.nominalFlightSeconds)
                || definition.nominalFlightSeconds < MinimumFlightSeconds
                || definition.nominalFlightSeconds
                    > MaximumAuthoredFlightSeconds
                || definition.nominalFlightSeconds
                    > profile.maximumFlightSeconds
                || !finitePositive(definition.lipHeightMeters)
                || !std::isfinite(definition.landingDropMeters)
                || definition.landingDropMeters < 0.0) {
            return false;
        }
        if (index > 0
                && (profile.lines[index - 1].gapLengthMeters
                        >= definition.gapLengthMeters
                    || profile.lines[index - 1]
                        .coldThresholdMetersPerSecond
                        >= definition.coldThresholdMetersPerSecond
                    || profile.lines[index - 1].nominalFlightSeconds
                        >= definition.nominalFlightSeconds)) {
            return false;
        }
    }

    return orderedLaterals(profile.lines,
                           2.0 * profile.socketHalfWidthMeters);
}

const WorkoutGameGapJumpLineDefinition *WorkoutGameGapJumpGeometry::line(
        const WorkoutGameGapJumpGeometryProfile &profile,
        WorkoutGameGapJumpLine id)
{
    if (!validate(profile) || id == WorkoutGameGapJumpLine::None) return nullptr;
    for (const auto &definition : profile.lines) {
        if (definition.id == id) return &definition;
    }
    return nullptr;
}
