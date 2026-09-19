/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCourseConversion.h"

#include <algorithm>
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

std::vector<WorkoutGameInterval> generatedIntervals(
        const WorkoutGameDistanceCourse &course,
        const std::vector<WorkoutGameInterval> &sourceIntervals)
{
    std::vector<WorkoutGameInterval> intervals;
    intervals.reserve(sourceIntervals.size());
    std::size_t sectionIndex = 0;
    for (const WorkoutGameInterval &source : sourceIntervals) {
        const std::int64_t sourceEnd = source.startMs + source.durationMs;
        WorkoutGameInterval generated;
        generated.startMs = source.startMs;
        bool haveSection = false;
        while (sectionIndex < course.sections.size()
                && course.sections[sectionIndex].sourceStartMs < sourceEnd) {
            const WorkoutGameDistanceCourseSection &section =
                    course.sections[sectionIndex++];
            if (!haveSection) {
                generated.startWatts = section.targetStartWatts;
                haveSection = true;
            }
            generated.durationMs += section.nominalDurationMs;
            generated.endWatts = section.targetEndWatts;
        }
        if (!haveSection) return {};
        intervals.push_back(generated);
    }
    if (sectionIndex != course.sections.size()) return {};
    return intervals;
}

}

WorkoutGameDistanceCourseGenerationParameters
WorkoutGameCourseConverter::parametersForPreset(
        WorkoutGameCoursePreset preset,
        const WorkoutGameRoadPhysicsParameters &roadPhysics)
{
    WorkoutGameDistanceCourseGenerationParameters parameters;
    parameters.roadPhysics = roadPhysics;
    parameters.simulationStepMs = 200;
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst:
        parameters.gradeScale = 0.70;
        parameters.technicality = 0.10;
        parameters.workMinimumDurationScale = 1.0;
        parameters.workMaximumDurationScale = 1.0;
        parameters.recoveryMinimumDurationScale = 1.0;
        parameters.recoveryMaximumDurationScale = 1.0;
        break;
    case WorkoutGameCoursePreset::Balanced:
        parameters.gradeScale = 1.00;
        parameters.technicality = 0.55;
        parameters.workMinimumDurationScale = 1.0;
        parameters.workMaximumDurationScale = 1.0;
        parameters.recoveryMinimumDurationScale = 1.0;
        parameters.recoveryMaximumDurationScale = 1.0;
        break;
    case WorkoutGameCoursePreset::RideFirst:
        parameters.gradeScale = 1.30;
        parameters.technicality = 0.95;
        parameters.workMinimumDurationScale = 1.0;
        parameters.workMaximumDurationScale = 1.0;
        parameters.recoveryMinimumDurationScale = 1.0;
        parameters.recoveryMaximumDurationScale = 1.0;
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
    result.generationParameters.terrainVariationPercent =
            request.terrainVariationPercent;
    result.generationParameters.variationLengthMeters =
            request.variationLengthMeters;
    result.generationParameters.referenceGear = request.referenceGear;
    result.course = WorkoutGameDistanceCourseBuilder::build(
            request.intervals,
            request.ftpWatts,
            result.generationParameters,
            request.seed);
    if (result.course.status != WorkoutGameDistanceCourseStatus::Ready) {
        result.status = WorkoutGameCourseConversionStatus::GenerationFailed;
        return result;
    }
    const std::vector<WorkoutGameInterval> generated = generatedIntervals(
            result.course, request.intervals);
    if (generated.size() != request.intervals.size()) {
        result.course = WorkoutGameDistanceCourse();
        result.status = WorkoutGameCourseConversionStatus::GenerationFailed;
        return result;
    }
    const WorkoutGameCoursePrescriptionAudit prescription =
            WorkoutGameCoursePrescription::audit(
                request.intervals,
                generated,
                request.ftpWatts,
                request.preset,
                request.prescriptionMetadata);
    if (prescription.status != WorkoutGameCoursePrescriptionStatus::Ready) {
        result.course = WorkoutGameDistanceCourse();
        result.status = WorkoutGameCourseConversionStatus::GenerationFailed;
        return result;
    }
    if (!WorkoutGameCourseSummary::build(
                result.course, request.intervals, request.ftpWatts,
                request.preset, prescription, request.roadPhysics,
                result.summary)) {
        result.status = WorkoutGameCourseConversionStatus::EstimateFailed;
        return result;
    }

    result.status = WorkoutGameCourseConversionStatus::Ready;
    return result;
}
