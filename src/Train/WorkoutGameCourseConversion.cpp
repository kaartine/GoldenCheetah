/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCourseConversion.h"

#include <cmath>

namespace {

bool validPreset(WorkoutGameCoursePreset preset)
{
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst:
    case WorkoutGameCoursePreset::Balanced:
    case WorkoutGameCoursePreset::RideFirst:
        return true;
    }
    return false;
}

void countFeatures(
        const WorkoutGameDistanceCourse &course,
        WorkoutGameCourseConversionSummary &summary)
{
    for (const WorkoutGameDistanceCourseSection &section : course.sections) {
        switch (section.terrain) {
        case WorkoutGameTerrainKind::Roots:
        case WorkoutGameTerrainKind::RockGarden:
        case WorkoutGameTerrainKind::Rollers:
        case WorkoutGameTerrainKind::BunnyHop:
        case WorkoutGameTerrainKind::Berm:
        case WorkoutGameTerrainKind::Skinny:
            ++summary.technicalFeatureCount;
            break;
        case WorkoutGameTerrainKind::SmoothTrail:
        case WorkoutGameTerrainKind::Climb:
        case WorkoutGameTerrainKind::Drop:
            break;
        }
        switch (section.feature) {
        case WorkoutGameFeature::Climb:
            ++summary.climbCount;
            break;
        case WorkoutGameFeature::SprintJump:
            ++summary.jumpCount;
            break;
        case WorkoutGameFeature::RecoveryDescent:
        case WorkoutGameFeature::CooldownDescent:
            ++summary.descentCount;
            break;
        default:
            break;
        }
    }
}

}

WorkoutGameDistanceCourseGenerationParameters
WorkoutGameCourseConverter::parametersForPreset(
        WorkoutGameCoursePreset preset,
        const WorkoutGameRoadPhysicsParameters &roadPhysics)
{
    WorkoutGameDistanceCourseGenerationParameters parameters;
    parameters.roadPhysics = roadPhysics;
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst:
        parameters.gradeScale = 0.82;
        parameters.technicality = 0.15;
        parameters.workMinimumDurationScale = 0.95;
        parameters.workMaximumDurationScale = 1.15;
        parameters.recoveryMinimumDurationScale = 0.8;
        parameters.recoveryMaximumDurationScale = 1.35;
        break;
    case WorkoutGameCoursePreset::Balanced:
        parameters.technicality = 0.55;
        break;
    case WorkoutGameCoursePreset::RideFirst:
        parameters.gradeScale = 1.18;
        parameters.technicality = 0.95;
        parameters.workMinimumDurationScale = 0.8;
        parameters.workMaximumDurationScale = 1.5;
        parameters.recoveryMinimumDurationScale = 0.55;
        parameters.recoveryMaximumDurationScale = 1.9;
        break;
    }
    return parameters;
}

WorkoutGameCourseConversionResult WorkoutGameCourseConverter::convert(
        const WorkoutGameCourseConversionRequest &request)
{
    WorkoutGameCourseConversionResult result;
    result.preset = request.preset;
    if (request.intervals.empty()
            || !std::isfinite(request.ftpWatts)
            || request.ftpWatts <= 0.0
            || !validPreset(request.preset)
            || !WorkoutGameRoadPhysics::validParameters(request.roadPhysics)) {
        return result;
    }

    result.generationParameters = parametersForPreset(
            request.preset, request.roadPhysics);
    result.course = WorkoutGameDistanceCourseBuilder::build(
            request.intervals,
            request.ftpWatts,
            result.generationParameters,
            request.seed);
    if (result.course.status != WorkoutGameDistanceCourseStatus::Ready) {
        result.status = WorkoutGameCourseConversionStatus::GenerationFailed;
        return result;
    }

    result.summary.nominalDurationMs = result.course.nominalDurationMs;
    result.summary.distanceMeters = result.course.totalDistanceMeters;
    result.summary.elevationGainMeters = result.course.elevationGainMeters;
    result.summary.elevationLossMeters = result.course.elevationLossMeters;
    countFeatures(result.course, result.summary);
    result.summary.fastEstimate = WorkoutGameDistanceCourseEstimator::estimate(
            result.course, request.roadPhysics, 1.15);
    result.summary.nominalEstimate = WorkoutGameDistanceCourseEstimator::estimate(
            result.course, request.roadPhysics, 1.0);
    result.summary.slowEstimate = WorkoutGameDistanceCourseEstimator::estimate(
            result.course, request.roadPhysics, 0.85);
    if (!result.summary.fastEstimate.finished
            || !result.summary.nominalEstimate.finished
            || !result.summary.slowEstimate.finished) {
        result.status = WorkoutGameCourseConversionStatus::EstimateFailed;
        return result;
    }

    result.status = WorkoutGameCourseConversionStatus::Ready;
    return result;
}
