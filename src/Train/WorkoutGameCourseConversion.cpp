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
        const WorkoutGameDistanceCourse &course)
{
    std::vector<WorkoutGameInterval> intervals;
    intervals.reserve(course.sections.size());
    std::int64_t startMs = 0;
    for (const WorkoutGameDistanceCourseSection &section : course.sections) {
        intervals.push_back({
            startMs,
            section.nominalDurationMs,
            section.targetStartWatts,
            section.targetEndWatts
        });
        startMs += section.nominalDurationMs;
    }
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
        parameters.gradeScale = 0.82;
        parameters.technicality = 0.15;
        parameters.workMinimumDurationScale = 1.0;
        parameters.workMaximumDurationScale = 1.0;
        parameters.recoveryMinimumDurationScale = 1.0;
        parameters.recoveryMaximumDurationScale = 1.0;
        break;
    case WorkoutGameCoursePreset::Balanced:
        parameters.technicality = 0.55;
        parameters.workMinimumDurationScale = 1.0;
        parameters.workMaximumDurationScale = 1.03;
        parameters.recoveryMinimumDurationScale = 1.0;
        parameters.recoveryMaximumDurationScale = 1.03;
        break;
    case WorkoutGameCoursePreset::RideFirst:
        parameters.gradeScale = 1.18;
        parameters.technicality = 0.95;
        parameters.workMinimumDurationScale = 1.0;
        parameters.workMaximumDurationScale = 1.08;
        parameters.recoveryMinimumDurationScale = 1.0;
        parameters.recoveryMaximumDurationScale = 1.08;
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
    for (std::size_t index = 0; index < request.intervals.size(); ++index) {
        const WorkoutGameCourseIntervalRole role =
                WorkoutGameCoursePrescription::roleAt(
                    request.prescriptionMetadata, index);
        if (request.preset == WorkoutGameCoursePreset::WorkoutFirst
                || role == WorkoutGameCourseIntervalRole::Prescribed) {
            result.course.sections[index].minimumDurationMs =
                    result.course.sections[index].nominalDurationMs;
            result.course.sections[index].maximumDurationMs = std::max(
                    result.course.sections[index].maximumDurationMs,
                    result.course.sections[index].nominalDurationMs);
        }
    }

    const WorkoutGameCoursePrescriptionAudit prescription =
            WorkoutGameCoursePrescription::audit(
                request.intervals,
                generatedIntervals(result.course),
                request.ftpWatts,
                request.preset,
                request.prescriptionMetadata);
    if (prescription.status != WorkoutGameCoursePrescriptionStatus::Ready) {
        result.course = WorkoutGameDistanceCourse();
        result.status = WorkoutGameCourseConversionStatus::GenerationFailed;
        return result;
    }
    const WorkoutGameCourseModeContract contract =
            WorkoutGameCoursePrescription::contractFor(request.preset);
    for (std::size_t index = 0; index < request.intervals.size(); ++index) {
        if (WorkoutGameCoursePrescription::isRecovery(
                    request.intervals[index], request.ftpWatts)
                && double(result.course.sections[index].minimumDurationMs) + 1.0
                    < double(request.intervals[index].durationMs)
                        * contract.minimumRecoveryExposure) {
            result.course = WorkoutGameDistanceCourse();
            result.status = WorkoutGameCourseConversionStatus::GenerationFailed;
            return result;
        }
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
