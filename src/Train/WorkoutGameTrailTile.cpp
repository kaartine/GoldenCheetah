/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameTrailTile.h"

#include "WorkoutGameAssetPhysicsSampler.h"

#include <algorithm>
#include <cmath>

namespace {

double halfWidthAt(
        const WorkoutGameRoadCourse &course,
        double distanceMeters)
{
    const WorkoutGameRoadSample sample =
            WorkoutGameRoadCourseBuilder::sample(course, distanceMeters);
    return sample.ready ? sample.center.halfWidthMeters : 0.0;
}

void appendTrailSpan(
        WorkoutGameTrailTile &tile,
        const WorkoutGameRoadCourse &course,
        double startDistanceMeters,
        double endDistanceMeters)
{
    constexpr double MaximumSpanMeters = 1.5;
    const double length = endDistanceMeters - startDistanceMeters;
    if (length <= 1e-6) return;
    const int count = std::max(1, int(std::ceil(length / MaximumSpanMeters)));
    for (int index = 0; index < count; ++index) {
        const double start = startDistanceMeters
                + length * double(index) / double(count);
        const double end = startDistanceMeters
                + length * double(index + 1) / double(count);
        WorkoutGameMeshInstance instance;
        instance.mesh = WorkoutGameMeshLibrary::trailTile(
                end - start,
                halfWidthAt(course, start),
                halfWidthAt(course, end),
                0.0);
        instance.anchorDistanceMeters = start;
        instance.renderLayer = WorkoutGameMeshRenderLayer::TrailSurface;
        instance.anchorToBaseSurface = false;
        tile.mainLine.push_back(std::move(instance));
    }
}

}

WorkoutGameTrailTile WorkoutGameTrailTileAssembler::challenge(
        const WorkoutGameRoadCourse &course,
        const WorkoutGameRoadPiece &piece)
{
    WorkoutGameTrailTile result;
    if (!course.ready || !piece.challenge.enabled
            || piece.lengthMeters <= 0.0) return result;
    WorkoutGameMesh feature = WorkoutGameMeshLibrary::feature(
            piece.terrain, piece.difficulty);
    if (!WorkoutGameMeshLibrary::valid(feature)) return result;

    double forwardScale = 1.0;
    double upScale = 1.0;
    if (course.assetPhysicsSnapshot) {
        const auto location = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [&piece](const WorkoutGameRoadPiece &candidate) {
                    return &candidate == &piece;
                });
        const std::size_t pieceIndex = std::size_t(
                std::distance(course.pieces.begin(), location));
        const WorkoutGameAssetRenderFit fit =
                WorkoutGameAssetPhysicsSampler::renderFit(
                    *course.assetPhysicsSnapshot, pieceIndex);
        if (fit.status == WorkoutGameAssetRenderFitStatus::Invalid) return {};
        if (fit.status == WorkoutGameAssetRenderFitStatus::Ready) {
            if (piece.terrain != WorkoutGameTerrainKind::LogOver
                    || fit.assetId
                        != QStringLiteral("FT-02-log-over-greybox")) {
                return {};
            }
            const double nativeLength =
                    feature.exit.forwardMeters - feature.entry.forwardMeters;
            double nativeHeight = 0.0;
            for (const WorkoutGameMeshVertex &vertex : feature.vertices) {
                nativeHeight = std::max(nativeHeight, vertex.upMeters);
            }
            const double resolvedExtentMeters =
                    double(fit.resolvedExtentMm) / 1000.0;
            if (nativeLength <= 0.0 || nativeHeight <= 0.0) return {};
            forwardScale = resolvedExtentMeters / nativeLength;
            upScale = resolvedExtentMeters / nativeHeight;
        }
    }

    const double featureStart = piece.challenge.obstacleDistanceMeters
            + feature.entry.forwardMeters * forwardScale;
    const double featureEnd = piece.challenge.obstacleDistanceMeters
            + feature.exit.forwardMeters * forwardScale;
    result.entryDistanceMeters = std::clamp(
            piece.challenge.bypassStartDistanceMeters,
            0.0, featureStart);
    result.exitDistanceMeters = std::clamp(
            piece.challenge.bypassEndDistanceMeters,
            featureEnd, course.totalLengthMeters);
    if (result.exitDistanceMeters - result.entryDistanceMeters <= 0.01) {
        return {};
    }
    result.sourceSectionIndex = piece.sourceSectionIndex;
    result.entryHalfWidthMeters = halfWidthAt(
            course, result.entryDistanceMeters);
    result.exitHalfWidthMeters = halfWidthAt(
            course, result.exitDistanceMeters);

    appendTrailSpan(result, course, result.entryDistanceMeters, featureStart);
    // The feature mesh may contain only the obstacle itself. Keep a complete
    // dirt bed underneath it so narrow props cannot reveal the background.
    appendTrailSpan(result, course, featureStart, featureEnd);
    WorkoutGameMeshInstance featureInstance;
    featureInstance.mesh = std::move(feature);
    featureInstance.anchorDistanceMeters =
            piece.challenge.obstacleDistanceMeters;
    featureInstance.forwardScale = forwardScale;
    featureInstance.upScale = upScale;
    featureInstance.entryRightScale = halfWidthAt(course, featureStart)
            / featureInstance.mesh.entry.halfWidthMeters;
    featureInstance.exitRightScale = halfWidthAt(course, featureEnd)
            / featureInstance.mesh.exit.halfWidthMeters;
    featureInstance.anchorToBaseSurface = true;
    featureInstance.occlusionAllowancePixels = 18.0;
    result.mainLine.push_back(std::move(featureInstance));
    appendTrailSpan(result, course, featureEnd, result.exitDistanceMeters);

    result.bypass.mesh = WorkoutGameMeshLibrary::bypassRibbon(
            result.exitDistanceMeters - result.entryDistanceMeters,
            piece.challenge.bypassLateralMeters,
            0.38,
            result.entryHalfWidthMeters,
            result.exitHalfWidthMeters);
    result.bypass.anchorDistanceMeters = result.entryDistanceMeters;
    result.bypass.renderLayer = WorkoutGameMeshRenderLayer::TrailSurface;
    result.bypass.anchorToBaseSurface = false;
    result.ready = !result.mainLine.empty()
            && WorkoutGameMeshLibrary::valid(result.bypass.mesh);
    return result;
}
