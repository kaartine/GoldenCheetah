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

constexpr std::int64_t FeatureDurationMs = 12000;
constexpr std::int64_t RecoveryDurationMs = 4000;

WorkoutGameSection featureSection(
        WorkoutGameTerrainKind terrain,
        double targetWatts,
        double gradePercent,
        double difficulty,
        std::uint32_t variant)
{
    WorkoutGameSection section;
    section.feature = terrain == WorkoutGameTerrainKind::Tabletop
            || terrain == WorkoutGameTerrainKind::LogOver
            ? WorkoutGameFeature::SprintJump
            : WorkoutGameFeature::Trail;
    section.terrain = terrain;
    section.durationMs = FeatureDurationMs;
    section.targetWatts = targetWatts;
    section.gradePercent = gradePercent;
    section.difficulty = difficulty;
    section.challengeCount = 1;
    section.visualVariant = variant;
    section.gravityAssisted = terrain == WorkoutGameTerrainKind::Drop;
    return section;
}

WorkoutGameSection recoverySection(double ftpWatts, std::uint32_t variant)
{
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::FlowTrail;
    section.terrain = WorkoutGameTerrainKind::SmoothTrail;
    section.durationMs = RecoveryDurationMs;
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
                       ftpWatts * 0.55, -6.0, 0.58, 5u)
    };

    std::int64_t startMs = 0;
    for (std::size_t index = 0; index < std::size(features); ++index) {
        WorkoutGameSection feature = features[index];
        feature.startMs = startMs;
        result.sections.push_back(feature);
        startMs += feature.durationMs;
        if (index + 1 < std::size(features)) {
            WorkoutGameSection recovery = recoverySection(
                    ftpWatts, std::uint32_t(index + 20));
            recovery.startMs = startMs;
            result.sections.push_back(recovery);
            startMs += recovery.durationMs;
        }
    }
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
