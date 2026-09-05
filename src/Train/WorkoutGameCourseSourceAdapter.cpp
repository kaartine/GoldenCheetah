/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCourseSourceAdapter.h"
#include "WorkoutGameDistancePlayback.h"
#include "WorkoutGameRoadCourse.h"
#include "WorkoutGameRoadPlan.h"
#include "WorkoutGameRoadQuality.h"

#include <QCryptographicHash>
#include <QFileInfo>

#include <cmath>
#include <memory>

namespace {

bool validSourceName(const QString &name)
{
    return !name.isEmpty()
            && name != QStringLiteral(".")
            && name != QStringLiteral("..")
            && name.size() <= 255
            && !name.contains(QLatin1Char('\n'))
            && !name.contains(QLatin1Char('\r'));
}

bool validTitle(const QString &title)
{
    return !title.trimmed().isEmpty()
            && title.size() <= 200
            && !title.contains(QLatin1Char('\n'))
            && !title.contains(QLatin1Char('\r'));
}

bool attachRoadPlan(
        WorkoutGameDistanceCourse &course,
        const std::vector<WorkoutGameInterval> &sourceIntervals,
        double ftpWatts,
        WorkoutGameCoursePreset preset)
{
    const WorkoutGameCourse visual =
            WorkoutGameDistancePlayback::visualCourse(course);
    const WorkoutGameRoadPlan plan =
            WorkoutGameRoadCourseBuilder::generatePlan(
                visual, ftpWatts, {
                    WorkoutGameRoadCourseGenerationParameters::CurrentVersion,
                    preset
                });
    if (WorkoutGameRoadPlanValidator::validate(plan, course.sections.size())
            != WorkoutGameRoadPlanValidationStatus::Ready
            || !WorkoutGameRoadQuality::audit(plan).accepted()) {
        return false;
    }
    for (const WorkoutGameRoadPiece &piece : plan.pieces) {
        if (piece.sourceSectionIndex >= sourceIntervals.size()) return false;
        if (WorkoutGameCoursePrescription::isRecovery(
                    sourceIntervals[piece.sourceSectionIndex], ftpWatts)
                && piece.challenge.enabled) {
            return false;
        }
    }
    course.roadPlan = std::make_shared<const WorkoutGameRoadPlan>(plan);
    return true;
}

}

WorkoutGameCourseSourceResult WorkoutGameCourseSourceAdapter::convert(
        const WorkoutGameCourseSourceRequest &request)
{
    WorkoutGameCourseSourceResult result;
    if (!std::isfinite(request.ftpWatts)
            || request.ftpWatts <= 0.0
            || request.ftpWatts > 3000.0) {
        result.status = WorkoutGameCourseSourceStatus::InvalidFtp;
        return result;
    }

    const QString sourceName = QFileInfo(request.sourceFileName).fileName();
    if (request.sourceContents.isEmpty()
            || request.sourceContents.size() > MaximumSourceBytes
            || !validSourceName(sourceName)) {
        return result;
    }
    const QString title = request.title.isEmpty()
            ? QFileInfo(sourceName).completeBaseName()
                + QStringLiteral(" MTB")
            : request.title;
    if (!validTitle(title)) return result;

    const WorkoutGameWorkout workout =
            WorkoutGameWorkoutAdapter::normalize(request.points);
    if (workout.status != WorkoutGameWorkoutStatus::Ready) {
        result.status = WorkoutGameCourseSourceStatus::InvalidWorkout;
        return result;
    }

    WorkoutGameCourseConversionRequest conversionRequest;
    conversionRequest.intervals = workout.intervals;
    conversionRequest.ftpWatts = request.ftpWatts;
    conversionRequest.preset = request.preset;
    conversionRequest.roadPhysics = request.roadPhysics;
    conversionRequest.prescriptionMetadata = request.prescriptionMetadata;
    conversionRequest.seed = request.seed;
    WorkoutGameCourseConversionResult conversion =
            WorkoutGameCourseConverter::convert(conversionRequest);
    if (conversion.status != WorkoutGameCourseConversionStatus::Ready) {
        result.status = WorkoutGameCourseSourceStatus::ConversionFailed;
        return result;
    }
    if (!attachRoadPlan(
                conversion.course, workout.intervals, request.ftpWatts,
                request.preset)) {
        result.status = WorkoutGameCourseSourceStatus::ConversionFailed;
        return result;
    }

    result.document.schemaVersion =
            WorkoutGameCourseDocumentCodec::CurrentSchemaVersion;
    result.document.title = title;
    result.document.sourceFileName = sourceName;
    result.document.sourceSha256 = QString::fromLatin1(
            QCryptographicHash::hash(
                request.sourceContents, QCryptographicHash::Sha256).toHex());
    result.document.sourceIntervals = workout.intervals;
    result.document.sourceLaps = request.sourceLaps;
    result.document.sourceTexts = request.sourceTexts;
    result.document.prescriptionMetadata = request.prescriptionMetadata;
    result.document.ftpWatts = request.ftpWatts;
    result.document.preset = request.preset;
    result.document.generationParameters = conversion.generationParameters;
    result.document.course = conversion.course;
    if (!WorkoutGameCourseDocumentCodec::valid(result.document)) {
        result.document = WorkoutGameCourseDocument();
        return result;
    }

    result.summary = conversion.summary;
    result.status = WorkoutGameCourseSourceStatus::Ready;
    return result;
}

WorkoutGameCourseSourceResult WorkoutGameCourseSourceAdapter::regenerate(
        const WorkoutGameCourseDocument &source,
        WorkoutGameCoursePreset preset,
        const QString &title)
{
    WorkoutGameCourseSourceResult result;
    if (source.sourceIntervals.empty()
            || !validTitle(title)
            || !WorkoutGameCourseDocumentCodec::valid(source)) {
        return result;
    }

    WorkoutGameCourseConversionRequest request;
    request.intervals = source.sourceIntervals;
    request.ftpWatts = source.ftpWatts;
    request.preset = preset;
    request.roadPhysics = source.generationParameters.roadPhysics;
    request.prescriptionMetadata = source.prescriptionMetadata;
    request.seed = source.course.seed;
    WorkoutGameCourseConversionResult conversion =
            WorkoutGameCourseConverter::convert(request);
    if (conversion.status != WorkoutGameCourseConversionStatus::Ready) {
        result.status = WorkoutGameCourseSourceStatus::ConversionFailed;
        return result;
    }
    if (!attachRoadPlan(
                conversion.course, source.sourceIntervals, source.ftpWatts,
                preset)) {
        result.status = WorkoutGameCourseSourceStatus::ConversionFailed;
        return result;
    }

    result.document = source;
    result.document.schemaVersion =
            WorkoutGameCourseDocumentCodec::CurrentSchemaVersion;
    result.document.conversionAlgorithmVersion =
            WorkoutGameCourseDocument::CurrentConversionAlgorithmVersion;
    result.document.title = title;
    result.document.preset = preset;
    result.document.generationParameters = conversion.generationParameters;
    result.document.course = conversion.course;
    result.summary = conversion.summary;
    result.status = WorkoutGameCourseSourceStatus::Ready;
    return result;
}
