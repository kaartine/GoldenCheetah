/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameFeatureLab.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

constexpr std::int64_t FeatureDurationMs = 5000;
constexpr std::int64_t TabletopDurationMs = 12000;
constexpr std::int64_t DropDurationMs = 7000;
constexpr std::int64_t SkinnyDurationMs = 8000;
constexpr std::int64_t RecoveryDurationMs = 1000;
constexpr std::int64_t FlowTransitionDurationMs = 2000;
constexpr std::int64_t FinalRunoutDurationMs = 5000;
constexpr double FeatureLengthMeters = 28.0;
constexpr double DropLengthMeters = 46.0;
constexpr double SkinnyLengthMeters = 44.0;
constexpr double BermLengthMeters = 36.0;
constexpr double TabletopLengthMeters = 84.0;
constexpr double RecoveryLengthMeters = 8.0;
constexpr double FlowTransitionLengthMeters = 22.0;
constexpr double FinalRunoutLengthMeters = 30.0;

WorkoutGameFeature featureForTerrain(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::Climb:
        return WorkoutGameFeature::Climb;
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver:
    case WorkoutGameTerrainKind::Tabletop:
        return WorkoutGameFeature::SprintJump;
    case WorkoutGameTerrainKind::Drop:
        return WorkoutGameFeature::RecoveryDescent;
    default:
        return WorkoutGameFeature::Trail;
    }
}

WorkoutGameSection featureSection(
        WorkoutGameTerrainKind terrain,
        double targetWatts,
        double gradePercent,
        double difficulty,
        std::uint32_t variant)
{
    WorkoutGameSection section;
    section.feature = featureForTerrain(terrain);
    section.terrain = terrain;
    section.durationMs = terrain == WorkoutGameTerrainKind::Tabletop
            ? TabletopDurationMs
            : terrain == WorkoutGameTerrainKind::Drop
            ? DropDurationMs
            : terrain == WorkoutGameTerrainKind::Skinny
            ? SkinnyDurationMs : FeatureDurationMs;
    section.lengthMeters = terrain == WorkoutGameTerrainKind::Tabletop
            ? TabletopLengthMeters
            : terrain == WorkoutGameTerrainKind::Drop
            ? DropLengthMeters
            : terrain == WorkoutGameTerrainKind::Skinny
            ? SkinnyLengthMeters
            : terrain == WorkoutGameTerrainKind::Berm
            ? BermLengthMeters : FeatureLengthMeters;
    section.targetWatts = targetWatts;
    section.gradePercent = gradePercent;
    section.difficulty = difficulty;
    section.challengeCount = 1;
    section.visualVariant = variant;
    section.reliefScale = terrain == WorkoutGameTerrainKind::Berm
            ? 1.35 + 0.25 * difficulty : 1.0;
    section.gravityAssisted = terrain == WorkoutGameTerrainKind::Drop;
    return section;
}

WorkoutGameSection flowTransitionSection(
        double ftpWatts,
        std::uint32_t variant)
{
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::FlowTrail;
    section.terrain = WorkoutGameTerrainKind::Rollers;
    section.durationMs = FlowTransitionDurationMs;
    section.lengthMeters = FlowTransitionLengthMeters;
    section.targetWatts = ftpWatts * 0.68;
    section.gradePercent = (variant & 1u) == 0u ? 5.0 : -7.0;
    section.difficulty = 0.82;
    section.reliefScale = 1.65;
    section.visualVariant = variant;
    section.gravityAssisted = section.gradePercent < 0.0;
    return section;
}

WorkoutGameSection recoverySection(
        double ftpWatts,
        std::uint32_t variant,
        std::int64_t durationMs = RecoveryDurationMs,
        double lengthMeters = RecoveryLengthMeters)
{
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::FlowTrail;
    section.terrain = WorkoutGameTerrainKind::SmoothTrail;
    section.durationMs = durationMs;
    section.lengthMeters = lengthMeters;
    section.targetWatts = ftpWatts * 0.55;
    section.gradePercent = -1.0;
    section.difficulty = 0.2;
    section.visualVariant = variant;
    section.gravityAssisted = true;
    return section;
}

const WorkoutGameSection *sectionAt(
        const WorkoutGameCourse &course,
        std::int64_t workoutTimeMs)
{
    if (workoutTimeMs < 0 || workoutTimeMs >= course.durationMs) {
        return nullptr;
    }
    for (const WorkoutGameSection &section : course.sections) {
        if (workoutTimeMs >= section.startMs
                && workoutTimeMs < section.startMs + section.durationMs) {
            return &section;
        }
    }
    return nullptr;
}

}

WorkoutGameCourse WorkoutGameFeatureLab::course(
        double ftpWatts,
        std::uint32_t seed)
{
    WorkoutGameCourse result;
    if (!std::isfinite(ftpWatts) || ftpWatts <= 0.0) {
        result.status = WorkoutGameCourseStatus::InvalidFtp;
        return result;
    }

    result.status = WorkoutGameCourseStatus::Ready;
    result.seed = seed;
    const WorkoutGameSection features[] = {
        featureSection(WorkoutGameTerrainKind::LogOver,
                       ftpWatts * 1.05, 0.0, 0.45, 1u),
        featureSection(WorkoutGameTerrainKind::Roots,
                       ftpWatts * 0.85, 1.0, 0.45, 2u),
        featureSection(WorkoutGameTerrainKind::RockGarden,
                       ftpWatts * 0.95, 2.0, 0.62, 3u),
        featureSection(WorkoutGameTerrainKind::Tabletop,
                       ftpWatts * 1.15, -1.0, 0.68, 4u),
        featureSection(WorkoutGameTerrainKind::Drop,
                       ftpWatts * 0.55, -6.0, 0.58, 5u),
        featureSection(WorkoutGameTerrainKind::Rollers,
                       ftpWatts * 0.82, 0.0, 0.42, 6u),
        featureSection(WorkoutGameTerrainKind::Climb,
                       ftpWatts * 1.10, 9.0, 0.72, 7u),
        featureSection(WorkoutGameTerrainKind::BunnyHop,
                       ftpWatts * 1.10, 0.0, 0.56, 8u),
        featureSection(WorkoutGameTerrainKind::Skinny,
                       ftpWatts * 0.78, 0.0, 0.60, 9u),
        featureSection(WorkoutGameTerrainKind::Berm,
                       ftpWatts * 0.84, 2.0, 0.10, 20u),
        featureSection(WorkoutGameTerrainKind::Berm,
                       ftpWatts * 0.88, -3.0, 0.40, 21u),
        featureSection(WorkoutGameTerrainKind::Berm,
                       ftpWatts * 0.92, 3.0, 0.70, 22u),
        featureSection(WorkoutGameTerrainKind::Berm,
                       ftpWatts * 0.96, -4.0, 1.00, 23u),
        featureSection(WorkoutGameTerrainKind::RockSlab,
                       ftpWatts * 0.92, -4.0, 0.70, 11u)
    };

    std::int64_t startMs = 0;
    for (std::size_t index = 0; index < std::size(features); ++index) {
        WorkoutGameSection feature = features[index];
        feature.startMs = startMs;
        result.sections.push_back(feature);
        startMs += feature.durationMs;
        if (index + 1 < std::size(features)) {
            const bool bermFlow = feature.terrain
                        == WorkoutGameTerrainKind::Berm
                    || features[index + 1].terrain
                        == WorkoutGameTerrainKind::Berm;
            WorkoutGameSection recovery = bermFlow
                    ? flowTransitionSection(
                        ftpWatts, std::uint32_t(index + 40))
                    : recoverySection(
                        ftpWatts, std::uint32_t(index + 20));
            recovery.startMs = startMs;
            result.sections.push_back(recovery);
            startMs += recovery.durationMs;
        }
    }
    WorkoutGameSection runout = recoverySection(
            ftpWatts, 31u, FinalRunoutDurationMs, FinalRunoutLengthMeters);
    runout.startMs = startMs;
    result.sections.push_back(runout);
    startMs += runout.durationMs;
    result.durationMs = startMs;
    return result;
}

double WorkoutGameFeatureLab::targetWattsAt(
        const WorkoutGameCourse &course,
        std::int64_t workoutTimeMs)
{
    const WorkoutGameSection *section = sectionAt(course, workoutTimeMs);
    return section ? section->targetWatts : 0.0;
}

WorkoutGameSimulationInput WorkoutGameFeatureLab::input(
        const WorkoutGameCourse &course,
        std::int64_t workoutTimeMs,
        WorkoutGameFeatureLabScenario scenario)
{
    WorkoutGameSimulationInput result;
    result.workoutTimeMs = std::clamp<std::int64_t>(
            workoutTimeMs, 0, std::max<std::int64_t>(0, course.durationMs));
    const WorkoutGameSection *section = sectionAt(course, result.workoutTimeMs);
    if (!section) return result;

    result.targetWatts = section->targetWatts;
    if (scenario == WorkoutGameFeatureLabScenario::Pass) {
        result.actualWatts = section->targetWatts;
        result.cadenceRpm = 88.0;
        result.authoritativeSpeedKph = 24.0;
        result.virtualGear = 8;
    } else {
        result.actualWatts = section->targetWatts * 0.42;
        result.cadenceRpm = 45.0;
        result.authoritativeSpeedKph = 4.0;
        result.virtualGear = 2;
    }
    return result;
}
