/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCourseSourceAdapter.h"

#include <QCryptographicHash>
#include <QFileInfo>

#include <cmath>

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
    conversionRequest.seed = request.seed;
    const WorkoutGameCourseConversionResult conversion =
            WorkoutGameCourseConverter::convert(conversionRequest);
    if (conversion.status != WorkoutGameCourseConversionStatus::Ready) {
        result.status = WorkoutGameCourseSourceStatus::ConversionFailed;
        return result;
    }

    result.document.title = title;
    result.document.sourceFileName = sourceName;
    result.document.sourceSha256 = QString::fromLatin1(
            QCryptographicHash::hash(
                request.sourceContents, QCryptographicHash::Sha256).toHex());
    result.document.sourceIntervals = workout.intervals;
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
    request.seed = source.course.seed;
    const WorkoutGameCourseConversionResult conversion =
            WorkoutGameCourseConverter::convert(request);
    if (conversion.status != WorkoutGameCourseConversionStatus::Ready) {
        result.status = WorkoutGameCourseSourceStatus::ConversionFailed;
        return result;
    }

    result.document = source;
    result.document.title = title;
    result.document.preset = preset;
    result.document.generationParameters = conversion.generationParameters;
    result.document.course = conversion.course;
    result.summary = conversion.summary;
    result.status = WorkoutGameCourseSourceStatus::Ready;
    return result;
}
