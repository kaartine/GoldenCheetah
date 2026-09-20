/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCourseSourceAdapter.h"
#include "WorkoutGameAssetCatalog.h"
#include "WorkoutGameAssetPhysicsResolver.h"
#include "WorkoutGameDistancePlayback.h"
#include "WorkoutGameRoadCourse.h"
#include "WorkoutGameRoadPlan.h"
#include "WorkoutGameRoadQuality.h"

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

const WorkoutGameAssetCatalog *assetCatalog()
{
    static const std::unique_ptr<const WorkoutGameAssetCatalog> catalog =
            WorkoutGameAssetCatalog::load();
    return catalog.get();
}

bool attachRoadPlan(
        WorkoutGameDistanceCourse &course,
        double ftpWatts,
        WorkoutGameCoursePreset preset)
{
    const WorkoutGameCourse visual =
            WorkoutGameDistancePlayback::visualCourse(course);
    WorkoutGameRoadPlan plan =
            WorkoutGameRoadCourseBuilder::generatePlan(
                visual, ftpWatts, {
                    WorkoutGameRoadCourseGenerationParameters::CurrentVersion,
                    preset
                });
    const WorkoutGameAssetCatalog *catalog = assetCatalog();
    if (!catalog) return false;
    const WorkoutGameAssetPhysicsResolution resolution =
            WorkoutGameAssetPhysicsResolver::resolve(*catalog, plan.pieces);
    if (resolution.status != WorkoutGameAssetPhysicsResolveStatus::Ready
            || !resolution.snapshot) {
        return false;
    }
    plan.assetPhysicsSnapshot = resolution.snapshot;
    if (WorkoutGameRoadPlanValidator::validate(plan, course.sections.size())
            != WorkoutGameRoadPlanValidationStatus::Ready
            || !plan.assetPhysicsSnapshot
            || !WorkoutGameRoadQuality::audit(plan).accepted()) {
        return false;
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
    conversionRequest.terrainVariationPercent =
            request.terrainVariationPercent;
    conversionRequest.variationLengthMeters = request.variationLengthMeters;
    conversionRequest.referenceGear = request.referenceGear;
    conversionRequest.seed = request.seed;
    WorkoutGameCourseConversionResult conversion =
            WorkoutGameCourseConverter::convert(conversionRequest);
    if (conversion.status != WorkoutGameCourseConversionStatus::Ready) {
        result.status = WorkoutGameCourseSourceStatus::ConversionFailed;
        return result;
    }
    if (!attachRoadPlan(
                conversion.course, request.ftpWatts, request.preset)) {
        result.status = WorkoutGameCourseSourceStatus::ConversionFailed;
        return result;
    }

    result.document.schemaVersion =
            WorkoutGameCourseDocumentCodec::CurrentSchemaVersion;
    result.document.title = title;
    result.document.sourceFileName = sourceName;
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
        const QString &title,
        double terrainVariationPercent,
        double variationLengthMeters)
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
    request.terrainVariationPercent = terrainVariationPercent >= 0.0
            ? terrainVariationPercent
            : source.generationParameters.terrainVariationPercent;
    request.variationLengthMeters = variationLengthMeters >= 0.0
            ? variationLengthMeters
            : source.generationParameters.variationLengthMeters;
    request.referenceGear = source.generationParameters.referenceGear;
    request.seed = source.course.seed;
    WorkoutGameCourseConversionResult conversion =
            WorkoutGameCourseConverter::convert(request);
    if (conversion.status != WorkoutGameCourseConversionStatus::Ready) {
        result.status = WorkoutGameCourseSourceStatus::ConversionFailed;
        return result;
    }
    if (!attachRoadPlan(
                conversion.course, source.ftpWatts, preset)) {
        result.status = WorkoutGameCourseSourceStatus::ConversionFailed;
        return result;
    }

    result.document = source;
    result.document.schemaVersion =
            WorkoutGameCourseDocumentCodec::CurrentSchemaVersion;
    result.document.sourceSha256.clear();
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
