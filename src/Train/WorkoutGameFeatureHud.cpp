/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameFeatureHud.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double PrepareLeadMeters = 12.0;
constexpr double ResultDistanceMeters = 6.0;

double finiteNonNegative(double value)
{
    return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

int readinessPercent(double value)
{
    if (!std::isfinite(value)) return 0;
    return int(std::lround(std::clamp(value, 0.0, 1.0) * 100.0));
}

bool bypassed(const WorkoutGameFeatureRuntimeSnapshot &feature)
{
    if (feature.terrain == WorkoutGameTerrainKind::Climb) return false;
    return feature.route == WorkoutGameRoute::SafeBypass
            || feature.outcome == WorkoutGameFeatureOutcome::Bypassed;
}

}

WorkoutGameFeatureHudSnapshot WorkoutGameFeatureHud::build(
        const WorkoutGameFeatureRuntimeSnapshot &feature,
        const WorkoutGameSimulationSnapshot &simulation,
        double targetWatts)
{
    WorkoutGameFeatureHudSnapshot result;
    if (!feature.ready || feature.phase == WorkoutGameFeaturePhase::None) {
        return result;
    }

    const double visualDistance = finiteNonNegative(feature.visualDistanceMeters);
    if (feature.phase == WorkoutGameFeaturePhase::Approach
            && finiteNonNegative(feature.prepareDistanceMeters)
                    > visualDistance + PrepareLeadMeters) {
        return result;
    }
    if (feature.phase == WorkoutGameFeaturePhase::Recovery
            && visualDistance
                    > finiteNonNegative(feature.actionEndDistanceMeters)
                            + ResultDistanceMeters) {
        return result;
    }

    result.visible = true;
    result.terrain = feature.terrain;
    result.route = feature.route;
    switch (feature.phase) {
    case WorkoutGameFeaturePhase::Approach:
        result.state = WorkoutGameFeatureHudState::Prepare;
        result.distanceKind = WorkoutGameFeatureHudDistanceKind::Decision;
        result.distanceMeters = std::max(
                0.0,
                finiteNonNegative(feature.decisionDistanceMeters)
                    - finiteNonNegative(feature.visualDistanceMeters));
        break;
    case WorkoutGameFeaturePhase::Measure:
        if (feature.terrain == WorkoutGameTerrainKind::GapJump
                && !feature.launchWindowActive) {
            result.state = WorkoutGameFeatureHudState::Prepare;
            result.distanceKind = WorkoutGameFeatureHudDistanceKind::Launch;
            result.distanceMeters = std::max(
                    0.0,
                    finiteNonNegative(
                        feature.launchWindowStartDistanceMeters)
                        - finiteNonNegative(feature.visualDistanceMeters));
        } else {
            result.state = feature.terrain
                        == WorkoutGameTerrainKind::GapJump
                    ? WorkoutGameFeatureHudState::Launch
                    : WorkoutGameFeatureHudState::Measure;
            result.distanceKind =
                    WorkoutGameFeatureHudDistanceKind::Decision;
            result.distanceMeters = std::max(
                    0.0,
                    finiteNonNegative(feature.decisionDistanceMeters)
                        - finiteNonNegative(feature.visualDistanceMeters));
        }
        break;
    case WorkoutGameFeaturePhase::Committed:
        result.state = WorkoutGameFeatureHudState::Committed;
        result.distanceKind = WorkoutGameFeatureHudDistanceKind::Action;
        result.distanceMeters = std::max(
                0.0,
                finiteNonNegative(feature.actionStartDistanceMeters)
                    - finiteNonNegative(feature.visualDistanceMeters));
        break;
    case WorkoutGameFeaturePhase::Action:
        result.state = bypassed(feature)
                ? WorkoutGameFeatureHudState::Bypass
                : WorkoutGameFeatureHudState::ActNow;
        break;
    case WorkoutGameFeaturePhase::Recovery:
        if (feature.outcome == WorkoutGameFeatureOutcome::Completed
                && !bypassed(feature)) {
            result.state = WorkoutGameFeatureHudState::Complete;
        } else if (feature.terrain == WorkoutGameTerrainKind::Climb
                && feature.outcome == WorkoutGameFeatureOutcome::Bypassed) {
            result.state = WorkoutGameFeatureHudState::NoBonus;
        } else if (bypassed(feature)) {
            result.state = WorkoutGameFeatureHudState::Bypass;
        } else {
            result.visible = false;
            result.state = WorkoutGameFeatureHudState::Hidden;
        }
        break;
    case WorkoutGameFeaturePhase::None:
        break;
    }

    const double effortRatio = simulation.challenge.minimumEffortRatio;
    const double normalizedTarget = finiteNonNegative(targetWatts);
    result.powerRequired = std::isfinite(effortRatio)
            && effortRatio > 0.0 && normalizedTarget > 0.0;
    if (result.powerRequired) {
        result.requiredPowerWatts = normalizedTarget * effortRatio;
    }
    result.powerReadinessPercent = feature.terrain
                == WorkoutGameTerrainKind::GapJump
            ? readinessPercent(
                double(feature.launchPowerHoldMilliseconds)
                    / WorkoutGameGapJumpLaunchWindow::
                        SpeedWindowDurationMilliseconds)
            : readinessPercent(
                simulation.challengeAssessment.effortReadiness);

    const double minimumCadence = simulation.challenge.minimumCadenceRpm;
    result.cadenceRequired = std::isfinite(minimumCadence)
            && minimumCadence > 0.0;
    if (result.cadenceRequired) {
        result.requiredCadenceRpm = minimumCadence;
    }
    result.cadenceReadinessPercent = readinessPercent(
            simulation.challengeAssessment.cadenceReadiness);
    return result;
}
