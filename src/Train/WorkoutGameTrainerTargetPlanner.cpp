/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameTrainerTargetPlanner.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double MaximumTrainerWatts = 1500.0;
constexpr double BalancedMaximumRelativeVariation = 0.08;
constexpr double BalancedMaximumAbsoluteVariationWatts = 20.0;

double finiteOr(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

double terrainEffortScale(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::Roots: return 0.40;
    case WorkoutGameTerrainKind::Rollers: return 0.30;
    case WorkoutGameTerrainKind::Climb: return 0.0;
    case WorkoutGameTerrainKind::RockGarden: return 0.60;
    case WorkoutGameTerrainKind::BunnyHop: return 0.80;
    case WorkoutGameTerrainKind::Skinny: return 0.25;
    case WorkoutGameTerrainKind::LogOver: return 0.85;
    case WorkoutGameTerrainKind::Tabletop: return 0.90;
    case WorkoutGameTerrainKind::RockSlab: return 0.55;
    case WorkoutGameTerrainKind::GapJump: return 1.00;
    case WorkoutGameTerrainKind::SmoothTrail:
    case WorkoutGameTerrainKind::Drop:
    case WorkoutGameTerrainKind::Berm:
        return 0.0;
    }
    return 0.0;
}

double effortDurationSeconds(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver:
        return 4.0;
    case WorkoutGameTerrainKind::Tabletop:
        return 6.0;
    case WorkoutGameTerrainKind::GapJump:
        return 8.0;
    default:
        return 6.0;
    }
}

double effortCentre(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver:
        return 0.58;
    case WorkoutGameTerrainKind::Rollers:
        return 0.82;
    case WorkoutGameTerrainKind::Roots:
    case WorkoutGameTerrainKind::RockGarden:
    case WorkoutGameTerrainKind::Skinny:
        return 0.85;
    case WorkoutGameTerrainKind::RockSlab:
        return 0.88;
    case WorkoutGameTerrainKind::Climb:
        return 0.95;
    case WorkoutGameTerrainKind::Tabletop:
    case WorkoutGameTerrainKind::GapJump:
        return 0.72;
    case WorkoutGameTerrainKind::SmoothTrail:
    case WorkoutGameTerrainKind::Drop:
    case WorkoutGameTerrainKind::Berm:
        return 0.5;
    }
    return 0.5;
}

}

bool WorkoutGameTrainerTargetPlanner::usesTargetPower(
        WorkoutGameCoursePreset preset,
        bool targetPowerSupported)
{
    return targetPowerSupported
            && preset != WorkoutGameCoursePreset::RideFirst;
}

double WorkoutGameTrainerTargetPlanner::workoutPowerWatts(
        WorkoutGameCoursePreset preset,
        double prescribedWatts,
        WorkoutGameTerrainKind terrain,
        double sectionProgress,
        std::int64_t sectionDurationMs)
{
    if (!std::isfinite(prescribedWatts) || prescribedWatts < 0.0) return -1.0;

    const double base = std::clamp(
            prescribedWatts, 0.0, MaximumTrainerWatts);
    if (preset != WorkoutGameCoursePreset::Balanced || base == 0.0) {
        return base;
    }

    const double maximumVariation = std::min(
            base * BalancedMaximumRelativeVariation,
            BalancedMaximumAbsoluteVariationWatts);
    return std::clamp(
            base + maximumVariation * terrainEffortSignal(
                terrain, sectionProgress, sectionDurationMs),
            0.0, MaximumTrainerWatts);
}

double WorkoutGameTrainerTargetPlanner::terrainEffortSignal(
        WorkoutGameTerrainKind terrain,
        double rawSectionProgress,
        std::int64_t sectionDurationMs)
{
    const double scale = terrainEffortScale(terrain);
    if (scale <= 0.0 || sectionDurationMs <= 0) return 0.0;

    const double progress = std::clamp(
            finiteOr(rawSectionProgress, 0.0), 0.0, 1.0);
    const double sectionSeconds = double(sectionDurationMs) / 1000.0;
    const double centre = effortCentre(terrain);
    const double maximumHalfWidth = std::min({0.24, centre, 1.0 - centre});
    const double halfWidth = std::clamp(
            effortDurationSeconds(terrain) / (2.0 * sectionSeconds),
            0.01, maximumHalfWidth);
    const double distance = std::abs(progress - centre);
    double pulse = 0.0;
    if (distance < halfWidth) {
        constexpr double Pi = 3.14159265358979323846;
        pulse = 0.5 * (1.0 + std::cos(Pi * distance / halfWidth));
    }

    // The raised-cosine pulse has area halfWidth over a unit section. The
    // small offset preserves the section's prescribed average workload.
    return scale * (pulse - halfWidth) / (1.0 - halfWidth);
}

TrainerTarget WorkoutGameTrainerTargetPlanner::plan(
        const WorkoutGameTrainerTargetInput &input)
{
    const double position = std::max(0.0, finiteOr(input.workoutPosition, 0.0));
    if (usesTargetPower(input.preset, input.targetPowerSupported)) {
        const double watts = workoutPowerWatts(
                input.preset, input.prescribedWatts, input.terrain,
                input.sectionProgress, input.sectionDurationMs);
        if (watts >= 0.0) return TrainerTarget::erg(watts, position);
    }

    return TrainerTarget::slope(
            std::clamp(finiteOr(input.gradePercent, 0.0), -40.0, 40.0),
            std::max(0.0, finiteOr(input.windResistance, 0.0)),
            position);
}
