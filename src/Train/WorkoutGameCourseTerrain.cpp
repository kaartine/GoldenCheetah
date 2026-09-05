/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCourseTerrain.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

WorkoutGameTerrainKind workoutFirstTerrain(std::size_t index)
{
    static constexpr WorkoutGameTerrainKind Palette[] = {
        WorkoutGameTerrainKind::Roots,
        WorkoutGameTerrainKind::Rollers,
        WorkoutGameTerrainKind::RockGarden,
        WorkoutGameTerrainKind::LogOver
    };
    return Palette[index % std::size(Palette)];
}

WorkoutGameTerrainKind balancedTerrain(std::size_t index)
{
    static constexpr WorkoutGameTerrainKind Palette[] = {
        WorkoutGameTerrainKind::Roots,
        WorkoutGameTerrainKind::RockGarden,
        WorkoutGameTerrainKind::Rollers,
        WorkoutGameTerrainKind::LogOver,
        WorkoutGameTerrainKind::Skinny,
        WorkoutGameTerrainKind::Roots
    };
    return Palette[index % std::size(Palette)];
}

WorkoutGameTerrainKind rideFirstTerrain(std::size_t index)
{
    static constexpr WorkoutGameTerrainKind Palette[] = {
        WorkoutGameTerrainKind::Skinny,
        WorkoutGameTerrainKind::RockGarden,
        WorkoutGameTerrainKind::RockSlab,
        WorkoutGameTerrainKind::Roots,
        WorkoutGameTerrainKind::Rollers,
        WorkoutGameTerrainKind::LogOver,
        WorkoutGameTerrainKind::Skinny,
        WorkoutGameTerrainKind::RockGarden,
        WorkoutGameTerrainKind::RockSlab
    };
    return Palette[index % std::size(Palette)];
}

double technicalShare(WorkoutGameCoursePreset preset)
{
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst: return 0.35;
    case WorkoutGameCoursePreset::Balanced: return 0.60;
    case WorkoutGameCoursePreset::RideFirst: return 0.90;
    }
    return 0.0;
}

void applyChallengeCount(WorkoutGameSection &section, bool sourceRecovery)
{
    if (sourceRecovery || section.feature == WorkoutGameFeature::RecoveryDescent
            || section.feature == WorkoutGameFeature::CooldownDescent) {
        section.challengeCount = 0;
        return;
    }
    section.challengeCount = section.terrain == WorkoutGameTerrainKind::SmoothTrail
            ? 0 : std::max(1, section.challengeCount);
}

}

bool WorkoutGameCourseTerrain::paletteEligible(WorkoutGameFeature feature)
{
    return feature == WorkoutGameFeature::WarmupTrail
            || feature == WorkoutGameFeature::Trail
            || feature == WorkoutGameFeature::FlowTrail;
}

void WorkoutGameCourseTerrain::apply(
        WorkoutGameSection &section,
        WorkoutGameCoursePreset preset,
        std::size_t paletteIndex,
        std::size_t paletteCount,
        std::uint32_t seed,
        bool sourceRecovery)
{
    if (sourceRecovery) {
        section.terrain = WorkoutGameTerrainKind::SmoothTrail;
        section.challengeCount = 0;
        return;
    }
    if (section.feature == WorkoutGameFeature::RecoveryDescent
            || section.feature == WorkoutGameFeature::CooldownDescent) {
        section.terrain = preset == WorkoutGameCoursePreset::RideFirst
                ? WorkoutGameTerrainKind::Berm
                : section.terrain;
        section.challengeCount = 0;
        return;
    }
    if (section.feature == WorkoutGameFeature::Climb) return;

    if (section.feature == WorkoutGameFeature::SprintJump) {
        if (preset == WorkoutGameCoursePreset::WorkoutFirst) {
            section.terrain = WorkoutGameTerrainKind::LogOver;
        } else if (preset == WorkoutGameCoursePreset::RideFirst) {
            switch ((std::size_t(section.visualVariant) + seed) % 3u) {
            case 0u: section.terrain = WorkoutGameTerrainKind::LogOver; break;
            case 1u: section.terrain = WorkoutGameTerrainKind::Tabletop; break;
            default: section.terrain = WorkoutGameTerrainKind::GapJump; break;
            }
        }
        applyChallengeCount(section, false);
        return;
    }
    if (!paletteEligible(section.feature)) return;

    if (paletteCount == 0u) {
        section.terrain = WorkoutGameTerrainKind::SmoothTrail;
        section.challengeCount = 0;
        return;
    }
    const std::size_t technicalCount = std::min(
            paletteCount,
            std::size_t(std::llround(
                technicalShare(preset) * double(paletteCount))));
    const std::size_t rotated = (paletteIndex
            + std::size_t(seed % std::uint32_t(paletteCount))) % paletteCount;
    const std::size_t before = rotated * technicalCount / paletteCount;
    const std::size_t after = (rotated + 1u) * technicalCount / paletteCount;
    if (after == before) {
        section.terrain = WorkoutGameTerrainKind::SmoothTrail;
        section.challengeCount = 0;
        return;
    }
    const std::size_t technicalOrdinal = after - 1u;
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst:
        section.terrain = workoutFirstTerrain(technicalOrdinal);
        break;
    case WorkoutGameCoursePreset::Balanced:
        section.terrain = balancedTerrain(technicalOrdinal);
        break;
    case WorkoutGameCoursePreset::RideFirst:
        section.terrain = rideFirstTerrain(technicalOrdinal);
        break;
    }
    applyChallengeCount(section, false);
}

double WorkoutGameCourseTerrain::curvatureDegreesPer100m(
        WorkoutGameCoursePreset preset)
{
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst: return 55.0;
    case WorkoutGameCoursePreset::Balanced: return 85.0;
    case WorkoutGameCoursePreset::RideFirst: return 125.0;
    }
    return 0.0;
}
