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
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <set>

namespace {

WorkoutGameTerrainKind workoutFirstTerrain(std::size_t index)
{
    static constexpr WorkoutGameTerrainKind Palette[] = {
        WorkoutGameTerrainKind::Roots,
        WorkoutGameTerrainKind::Rollers
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
    case WorkoutGameCoursePreset::WorkoutFirst: return 0.20;
    case WorkoutGameCoursePreset::Balanced: return 0.60;
    case WorkoutGameCoursePreset::RideFirst: return 0.90;
    }
    return 0.0;
}

std::uint32_t selectionHash(std::uint32_t seed, std::size_t index)
{
    std::uint32_t value = seed ^ std::uint32_t(index * 0x9e3779b9u);
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

struct TerrainCandidate
{
    double distanceMeters = 0.0;
    std::uint32_t tieBreaker = 0u;
    std::size_t index = 0u;
};

struct TerrainCandidateLess
{
    bool operator()(const TerrainCandidate &left,
                    const TerrainCandidate &right) const
    {
        if (left.distanceMeters != right.distanceMeters) {
            return left.distanceMeters < right.distanceMeters;
        }
        if (left.tieBreaker != right.tieBreaker) {
            return left.tieBreaker < right.tieBreaker;
        }
        return left.index < right.index;
    }
};

std::array<std::size_t, 3> nestedTechnicalCounts(std::size_t count)
{
    if (count < 2u) return {0u, 0u, 0u};
    const auto desired = [count](double share) {
        return std::size_t(std::llround(share * double(count)));
    };
    const std::size_t calm = std::min(
            desired(technicalShare(WorkoutGameCoursePreset::WorkoutFirst)),
            count - 2u);
    const std::size_t varied = std::clamp(
            desired(technicalShare(WorkoutGameCoursePreset::Balanced)),
            calm + 1u, count - 1u);
    const std::size_t technical = std::clamp(
            desired(technicalShare(WorkoutGameCoursePreset::RideFirst)),
            varied + 1u, count);
    return {calm, varied, technical};
}

std::size_t presetIndex(WorkoutGameCoursePreset preset)
{
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst: return 0u;
    case WorkoutGameCoursePreset::Balanced: return 1u;
    case WorkoutGameCoursePreset::RideFirst: return 2u;
    }
    return 0u;
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

std::vector<WorkoutGameCourseTerrainSelection>
WorkoutGameCourseTerrain::selectTechnicalTerrain(
        const std::vector<double> &eligibleDistancesMeters,
        WorkoutGameCoursePreset preset,
        std::uint32_t seed)
{
    const std::size_t count = eligibleDistancesMeters.size();
    std::vector<WorkoutGameCourseTerrainSelection> result(count);
    if (count < 2u) return result;

    double totalDistance = 0.0;
    for (double distance : eligibleDistancesMeters) {
        if (!std::isfinite(distance) || distance <= 0.0) return {};
        totalDistance += distance;
    }
    if (!std::isfinite(totalDistance) || totalDistance <= 0.0) return {};

    const std::array<std::size_t, 3> counts = nestedTechnicalCounts(count);
    const std::array<double, 3> targets {0.20, 0.60, 0.90};
    std::set<TerrainCandidate, TerrainCandidateLess> candidates;
    for (std::size_t index = 0; index < count; ++index) {
        candidates.insert({eligibleDistancesMeters[index],
                           selectionHash(seed, index), index});
    }
    std::vector<std::size_t> order;
    order.reserve(counts.back());
    double selectedDistance = 0.0;
    std::size_t selectedCount = 0u;
    for (std::size_t stage = 0; stage < counts.size(); ++stage) {
        while (selectedCount < counts[stage]) {
            if (candidates.empty()) return {};
            const std::size_t remainingPicks = counts[stage] - selectedCount;
            const double remainingTarget = targets[stage] * totalDistance
                    - selectedDistance;
            const double desiredDistance = std::max(
                    0.0, remainingTarget / double(remainingPicks));
            const TerrainCandidate probe {desiredDistance, 0u, 0u};
            auto upper = candidates.lower_bound(probe);
            auto best = candidates.end();
            double bestError = std::numeric_limits<double>::infinity();
            std::uint32_t bestTie = std::numeric_limits<std::uint32_t>::max();
            const auto consider = [&](auto candidate) {
                if (candidate == candidates.end()) return;
                const double error = std::abs(
                        candidate->distanceMeters - desiredDistance);
                if (error < bestError - 1.0e-12
                        || (std::abs(error - bestError) <= 1.0e-12
                            && candidate->tieBreaker < bestTie)) {
                    best = candidate;
                    bestError = error;
                    bestTie = candidate->tieBreaker;
                }
            };
            consider(upper);
            if (upper != candidates.begin()) consider(std::prev(upper));
            if (best == candidates.end()) return {};
            selectedDistance += best->distanceMeters;
            order.push_back(best->index);
            candidates.erase(best);
            ++selectedCount;
        }
    }

    const std::size_t requestedCount = counts[presetIndex(preset)];
    std::vector<bool> requested(count, false);
    for (std::size_t orderIndex = 0; orderIndex < requestedCount;
            ++orderIndex) {
        requested[order[orderIndex]] = true;
    }
    std::size_t ordinal = 0u;
    for (std::size_t index = 0; index < count; ++index) {
        if (requested[index]) {
            result[index] = {true, ordinal++};
        }
    }
    return result;
}

void WorkoutGameCourseTerrain::apply(
        WorkoutGameSection &section,
        WorkoutGameCoursePreset preset,
        const WorkoutGameCourseTerrainSelection &selection,
        std::uint32_t seed,
        bool sourceRecovery)
{
    if (sourceRecovery
            || section.feature == WorkoutGameFeature::RecoveryDescent
            || section.feature == WorkoutGameFeature::CooldownDescent) {
        section.terrain = WorkoutGameTerrainKind::SmoothTrail;
        section.challengeCount = 0;
        return;
    }
    if (section.feature == WorkoutGameFeature::Climb) return;

    if (section.feature == WorkoutGameFeature::SprintJump) {
        if (preset == WorkoutGameCoursePreset::WorkoutFirst) {
            section.terrain = WorkoutGameTerrainKind::Rollers;
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

    if (!selection.technical) {
        section.terrain = WorkoutGameTerrainKind::SmoothTrail;
        section.challengeCount = 0;
        return;
    }
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst:
        section.terrain = workoutFirstTerrain(selection.ordinal);
        break;
    case WorkoutGameCoursePreset::Balanced:
        section.terrain = balancedTerrain(selection.ordinal);
        break;
    case WorkoutGameCoursePreset::RideFirst:
        section.terrain = rideFirstTerrain(selection.ordinal);
        break;
    }
    applyChallengeCount(section, false);
}
