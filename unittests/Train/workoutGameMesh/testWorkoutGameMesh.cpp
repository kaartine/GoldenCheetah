/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameMesh.h"
#include "Train/WorkoutGameFeatureGeometry.h"
#include "Train/WorkoutGameForestFloor.h"
#include "Train/WorkoutGameRootGeometry.h"
#include "Train/WorkoutGameRockGardenGeometry.h"
#include "Train/WorkoutGameRockSlabGeometry.h"
#include "Train/WorkoutGameOcclusion.h"
#include "Train/WorkoutGameTrailBranch.h"
#include "Train/WorkoutGameTrailTile.h"

#include <QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace {

WorkoutGameRoadCourse straightCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 91u;
    course.durationMs = 20000;
    WorkoutGameSection section;
    section.durationMs = course.durationMs;
    section.targetWatts = 200.0;
    section.terrain = WorkoutGameTerrainKind::SmoothTrail;
    course.sections.push_back(section);
    return WorkoutGameRoadCourseBuilder::build(course, 200.0);
}

WorkoutGameRoadCourse featureCourse(
        WorkoutGameTerrainKind terrain,
        double difficulty)
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 414u;
    course.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::SprintJump;
    section.terrain = terrain;
    section.durationMs = course.durationMs;
    section.targetWatts = 240.0;
    section.difficulty = difficulty;
    section.challengeCount = 1;
    course.sections.push_back(section);
    return WorkoutGameRoadCourseBuilder::build(course, 200.0);
}

}

class TestWorkoutGameMesh : public QObject
{
    Q_OBJECT

private slots:
    void rockSlabMeshUsesCanonicalAsymmetricMassAndFissureBudget()
    {
        const WorkoutGameRockSlabGeometryProfile profile =
                WorkoutGameRockSlabGeometry::profile(0.65);
        const WorkoutGameMesh mesh = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::RockSlab, 0.65);
        QVERIFY(mesh.ready);
        QCOMPARE(mesh.entry.forwardMeters, profile.startMeters);
        QCOMPARE(mesh.exit.forwardMeters, profile.endMeters);
        QCOMPARE(mesh.entry.halfWidthMeters,
                 profile.socketHalfWidthMeters);
        QCOMPARE(mesh.exit.halfWidthMeters,
                 profile.socketHalfWidthMeters);
        QCOMPARE(mesh.lengthMeters, profile.endMeters - profile.startMeters);
        QVERIFY(mesh.vertices.size() <= 160u);
        QVERIFY(mesh.triangles.size() <= 228u);
        QVERIFY(mesh.colliders.size() >= 8u);

        double minimumRight = 0.0;
        double maximumRight = 0.0;
        double minimumUp = 0.0;
        double maximumUp = 0.0;
        int fissureTriangles = 0;
        for (const WorkoutGameMeshVertex &vertex : mesh.vertices) {
            minimumRight = std::min(minimumRight, vertex.rightMeters);
            maximumRight = std::max(maximumRight, vertex.rightMeters);
            minimumUp = std::min(minimumUp, vertex.upMeters);
            maximumUp = std::max(maximumUp, vertex.upMeters);
        }
        for (const WorkoutGameMeshTriangle &triangle : mesh.triangles) {
            if (triangle.material == WorkoutGameMeshMaterial::RockSide) {
                ++fissureTriangles;
            }
        }
        QVERIFY(minimumRight < -1.10);
        QVERIFY(maximumRight > 0.50);
        QVERIFY(minimumUp <= -profile.sideDepthMeters * 0.95);
        QVERIFY(maximumUp >= profile.heightMeters * 0.95);
        QVERIFY(fissureTriangles >= 6);
    }

    void rockGardenMeshUsesTheCanonicalBuriedStoneBudget()
    {
        const WorkoutGameRockGardenGeometryProfile profile =
                WorkoutGameRockGardenGeometry::profile(0.65);
        const WorkoutGameMesh mesh = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::RockGarden, 0.65);
        QVERIFY(mesh.ready);
        QCOMPARE(mesh.entry.forwardMeters, profile.startMeters);
        QCOMPARE(mesh.exit.forwardMeters, profile.endMeters);
        QCOMPARE(mesh.entry.halfWidthMeters,
                 profile.socketHalfWidthMeters);
        QCOMPARE(mesh.exit.halfWidthMeters,
                 profile.socketHalfWidthMeters);
        QCOMPARE(mesh.colliders.size(), profile.stones.size());
        QVERIFY(mesh.triangles.size() <= 300u);
        double minimumUp = 0.0;
        double maximumUp = 0.0;
        for (const WorkoutGameMeshVertex &vertex : mesh.vertices) {
            minimumUp = std::min(minimumUp, vertex.upMeters);
            maximumUp = std::max(maximumUp, vertex.upMeters);
        }
        QVERIFY(minimumUp < -0.02);
        QVERIFY(maximumUp >= 0.18);
    }

    void rootsMeshUsesTheCanonicalNetworkAndSocketBudget()
    {
        const WorkoutGameRootGeometryProfile profile =
                WorkoutGameRootGeometry::profile(0.65);
        const WorkoutGameMesh mesh = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::Roots, 0.65);
        QVERIFY(mesh.ready);
        QCOMPARE(mesh.entry.forwardMeters, profile.startMeters);
        QCOMPARE(mesh.exit.forwardMeters, profile.endMeters);
        QCOMPARE(mesh.entry.halfWidthMeters,
                 profile.socketHalfWidthMeters);
        QCOMPARE(mesh.exit.halfWidthMeters,
                 profile.socketHalfWidthMeters);
        QCOMPARE(mesh.colliders.size(), profile.segments.size());
        QVERIFY(mesh.triangles.size() <= 160u);
        QCOMPARE(mesh.triangles.size(), profile.segments.size() * 16u);
    }

    void featureModelsAreValidAndReadyForTexturesAndCollision()
    {
        const WorkoutGameTerrainKind terrains[] = {
            WorkoutGameTerrainKind::Rollers,
            WorkoutGameTerrainKind::Climb,
            WorkoutGameTerrainKind::LogOver,
            WorkoutGameTerrainKind::Roots,
            WorkoutGameTerrainKind::RockGarden,
            WorkoutGameTerrainKind::BunnyHop,
            WorkoutGameTerrainKind::Skinny,
            WorkoutGameTerrainKind::Berm,
            WorkoutGameTerrainKind::Tabletop,
            WorkoutGameTerrainKind::Drop,
            WorkoutGameTerrainKind::RockSlab
        };
        for (WorkoutGameTerrainKind terrain : terrains) {
            const WorkoutGameMesh model =
                    WorkoutGameMeshLibrary::feature(terrain, 0.7);
            QVERIFY2(WorkoutGameMeshLibrary::valid(model),
                     "feature mesh must have finite indexed triangles");
            QVERIFY2(model.vertices.size() >= 12u,
                     "feature still uses the old generic box silhouette");
            QVERIFY2(model.triangles.size() >= 8u,
                     "feature lacks a sculpted surface");
            QVERIFY(!model.colliders.empty());
            for (const WorkoutGameMeshVertex &vertex : model.vertices) {
                QVERIFY(std::isfinite(vertex.u));
                QVERIFY(std::isfinite(vertex.v));
            }
        }
    }

    void everyFeatureHasDepthAndHeightVariationInsteadOfABoxFallback()
    {
        const WorkoutGameTerrainKind terrains[] = {
            WorkoutGameTerrainKind::Rollers,
            WorkoutGameTerrainKind::Climb,
            WorkoutGameTerrainKind::Roots,
            WorkoutGameTerrainKind::RockGarden,
            WorkoutGameTerrainKind::BunnyHop,
            WorkoutGameTerrainKind::Drop,
            WorkoutGameTerrainKind::Skinny,
            WorkoutGameTerrainKind::Berm,
            WorkoutGameTerrainKind::LogOver,
            WorkoutGameTerrainKind::Tabletop,
            WorkoutGameTerrainKind::RockSlab
        };
        for (WorkoutGameTerrainKind terrain : terrains) {
            const WorkoutGameMesh mesh = WorkoutGameMeshLibrary::feature(
                    terrain, 0.65);
            std::vector<double> forward;
            std::vector<double> height;
            for (const WorkoutGameMeshVertex &vertex : mesh.vertices) {
                forward.push_back(vertex.forwardMeters);
                height.push_back(vertex.upMeters);
            }
            const auto uniqueCount = [](std::vector<double> values) {
                std::sort(values.begin(), values.end());
                values.erase(std::unique(
                    values.begin(), values.end(), [](double a, double b) {
                        return std::abs(a - b) < 1e-6;
                    }), values.end());
                return values.size();
            };
            QVERIFY2(uniqueCount(forward) >= 3u,
                     "feature has only the front and back faces of a box");
            QVERIFY2(uniqueCount(height) >= 3u,
                     "feature has only the top and bottom faces of a box");
        }
    }

    void bunnyHopUsesADistinctCompactHurdleInsteadOfTheLogMesh()
    {
        const WorkoutGameFeatureGeometryProfile bunny =
                WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::BunnyHop, 0.65);
        const WorkoutGameFeatureGeometryProfile log =
                WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::LogOver, 0.65);
        QVERIFY(bunny.ready && log.ready);
        QVERIFY(bunny.startMeters > log.startMeters);
        QVERIFY(bunny.endMeters < log.endMeters);
        QVERIFY(bunny.heightMeters < log.heightMeters);

        const WorkoutGameMesh bunnyMesh = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::BunnyHop, 0.65);
        const WorkoutGameMesh logMesh = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::LogOver, 0.65);
        QVERIFY(WorkoutGameMeshLibrary::valid(bunnyMesh));
        QVERIFY(WorkoutGameMeshLibrary::valid(logMesh));
        QVERIFY(bunnyMesh.lengthMeters < logMesh.lengthMeters);
        QVERIFY(bunnyMesh.vertices.size() != logMesh.vertices.size()
                || bunnyMesh.triangles.size() != logMesh.triangles.size());
    }

    void bunnyHopHurdleDoesNotRaiseTheAuthoritativeTrailSurface()
    {
        const WorkoutGameFeatureGeometryProfile easy =
                WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::BunnyHop, 0.0);
        const WorkoutGameFeatureGeometryProfile hard =
                WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::BunnyHop, 1.0);
        QVERIFY(easy.ready && hard.ready);
        QCOMPARE(easy.shape, WorkoutGameFeatureGeometryShape::Hurdle);
        QCOMPARE(easy.heightMeters, 0.10);
        QCOMPARE(hard.heightMeters, 0.20);
        QCOMPARE(easy.surfaceOffset(0.0), 0.0);
        QCOMPARE(hard.surfaceOffset(0.0), 0.0);
    }

    void rollersMeshMatchesTheCanonicalThreeCrestProfile()
    {
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::Rollers, 0.5);
        QVERIFY(profile.ready);
        const WorkoutGameMesh mesh = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::Rollers, 0.5);
        QVERIFY(WorkoutGameMeshLibrary::valid(mesh));
        QCOMPARE(mesh.entry.forwardMeters, profile.startMeters);
        QCOMPARE(mesh.exit.forwardMeters, profile.endMeters);
        QCOMPARE(mesh.entry.halfWidthMeters, 0.68);
        QCOMPARE(mesh.exit.halfWidthMeters, 0.68);
        for (double local : {-4.5, -3.0, -1.5, 0.0, 1.5, 3.0, 4.5}) {
            double visibleTop = -1.0;
            for (const WorkoutGameMeshVertex &vertex : mesh.vertices) {
                if (std::abs(vertex.forwardMeters - local) < 1e-9) {
                    visibleTop = std::max(visibleTop, vertex.upMeters);
                }
            }
            QVERIFY2(visibleTop >= 0.0,
                     qPrintable(QStringLiteral(
                         "roller mesh has no row at %1 m").arg(local)));
            QVERIFY2(std::abs(visibleTop - profile.surfaceOffset(local)) < 1e-9,
                     qPrintable(QStringLiteral(
                         "roller mesh/profile mismatch at %1 m: %2 vs %3")
                         .arg(local).arg(visibleTop)
                         .arg(profile.surfaceOffset(local))));
        }
    }

    void trailTilesExposeMatchingPuzzlePieceConnectors()
    {
        const WorkoutGameMesh first = WorkoutGameMeshLibrary::trailTile(
                12.0, 1.4, 1.8, 0.6);
        const WorkoutGameMesh second = WorkoutGameMeshLibrary::trailTile(
                9.0, 1.8, 1.2, -0.2);

        QVERIFY(WorkoutGameMeshLibrary::valid(first));
        QVERIFY(WorkoutGameMeshLibrary::valid(second));
        QCOMPARE(first.exit.halfWidthMeters, second.entry.halfWidthMeters);
        QCOMPARE(first.exit.forwardMeters, first.lengthMeters);
        QCOMPARE(second.entry.forwardMeters, 0.0);
    }

    void bypassRibbonLeavesAndReturnsToTheMainTrailSmoothly()
    {
        const WorkoutGameMesh bypass = WorkoutGameMeshLibrary::bypassRibbon(
                20.0, 1.5, 0.38);
        QVERIFY(WorkoutGameMeshLibrary::valid(bypass));
        QVERIFY(bypass.vertices.size() >= 20u);

        const double startRight = bypass.vertices[1].rightMeters
                + bypass.vertices[2].rightMeters;
        const double endRight = bypass.vertices[bypass.vertices.size() - 3].rightMeters
                + bypass.vertices[bypass.vertices.size() - 2].rightMeters;
        double largestCenter = 0.0;
        for (std::size_t index = 0; index + 3 < bypass.vertices.size(); index += 4) {
            largestCenter = std::max(largestCenter, std::abs(
                    (bypass.vertices[index + 1].rightMeters
                     + bypass.vertices[index + 2].rightMeters) * 0.5));
        }
        QVERIFY(std::abs(startRight) < 1e-9);
        QVERIFY(std::abs(endRight) < 1e-9);
        QVERIFY(largestCenter > 1.4);
    }

    void bypassMeshAndRuntimeCurveUseTheSameLateralFunction()
    {
        constexpr double Length = 18.0;
        constexpr double Lateral = -2.1;
        const WorkoutGameMesh bypass = WorkoutGameMeshLibrary::bypassRibbon(
                Length, Lateral, 0.38, 0.68, 0.68);
        QVERIFY(WorkoutGameMeshLibrary::valid(bypass));
        for (std::size_t index = 0; index + 3u < bypass.vertices.size();
             index += 4u) {
            const double distance = bypass.vertices[index].forwardMeters;
            const double center = (bypass.vertices[index + 1].rightMeters
                    + bypass.vertices[index + 2].rightMeters) * 0.5;
            QCOMPARE(center, WorkoutGameTrailBranch::lateralAt(
                    distance, 0.0, Length, Lateral));
        }
    }

    void forestPropsAndFloorShareOneCrossSection()
    {
        constexpr double Distance = 138.0;
        constexpr double TrailHalfWidth = 0.68;
        const double outer = WorkoutGameForestFloor::outerLateralMeters(
                TrailHalfWidth, false);
        const double outerHeight = WorkoutGameForestFloor::offsetMeters(
                Distance, outer, TrailHalfWidth);
        const double middle = TrailHalfWidth
                + WorkoutGameForestFloor::BlendWidthMeters * 0.5;
        QCOMPARE(WorkoutGameForestFloor::offsetMeters(
                    Distance, middle, TrailHalfWidth), outerHeight * 0.5);
        QCOMPARE(WorkoutGameForestFloor::offsetMeters(
                    Distance, outer + 2.0, TrailHalfWidth), outerHeight);

        WorkoutGameRoadProjectionFrame projection;
        projection.ready = true;
        projection.verticalExaggeration = 1.7;
        WorkoutGameRoadProjectedSlice far;
        far.worldDistanceMeters = 100.0;
        far.centerX = 700.0;
        far.centerY = 1000.0;
        far.halfWidthMeters = TrailHalfWidth;
        far.pixelsPerMeter = 2.0;
        WorkoutGameRoadProjectedSlice near = far;
        near.worldDistanceMeters = 20.0;
        near.centerX = 500.0;
        near.centerY = 400.0;
        near.pixelsPerMeter = 10.0;
        projection.slices = {far, near};
        const double screenX = 700.0;
        const double sightlineLateral =
                (screenX - near.centerX) / near.pixelsPerMeter;
        const double expected = near.centerY
                - WorkoutGameForestFloor::offsetMeters(
                    near.worldDistanceMeters,
                    sightlineLateral,
                    near.halfWidthMeters)
                    * near.pixelsPerMeter * projection.verticalExaggeration;
        const WorkoutGameForestFloorProjection forestProjection =
                WorkoutGameForestFloorProjection::build(projection);
        QVERIFY(forestProjection.isReady());
        QCOMPARE(forestProjection.occlusionY(
                    far.worldDistanceMeters, screenX), expected);
    }

    void challengeTileUsesOneSocketPairForFeatureAndBypass()
    {
        const WorkoutGameRoadCourse course = featureCourse(
                WorkoutGameTerrainKind::Tabletop, 0.7);
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());

        const WorkoutGameTrailTile tile =
                WorkoutGameTrailTileAssembler::challenge(course, *piece);
        QVERIFY(tile.ready);
        QVERIFY(tile.entryDistanceMeters
                < piece->challenge.obstacleDistanceMeters);
        QVERIFY(tile.exitDistanceMeters
                > piece->challenge.obstacleDistanceMeters);
        QCOMPARE(tile.entryDistanceMeters,
                 piece->challenge.bypassStartDistanceMeters);
        QCOMPARE(tile.exitDistanceMeters,
                 piece->challenge.bypassEndDistanceMeters);
        QVERIFY(!tile.mainLine.empty());
        QCOMPARE(tile.mainLine.front().mesh.entry.halfWidthMeters
                    * tile.mainLine.front().entryRightScale,
                 tile.entryHalfWidthMeters);
        QCOMPARE(tile.mainLine.back().mesh.exit.halfWidthMeters
                    * tile.mainLine.back().exitRightScale,
                 tile.exitHalfWidthMeters);
        QCOMPARE(tile.bypass.mesh.entry.halfWidthMeters,
                 tile.entryHalfWidthMeters);
        QCOMPARE(tile.bypass.mesh.exit.halfWidthMeters,
                 tile.exitHalfWidthMeters);
        QCOMPARE(tile.bypass.anchorDistanceMeters,
                 tile.entryDistanceMeters);
        QCOMPARE(tile.bypass.renderLayer,
                 WorkoutGameMeshRenderLayer::TrailSurface);
        QVERIFY(tile.bypass.clipToRoadOcclusion);

        double featureHalfWidth = 0.0;
        for (const WorkoutGameMeshInstance &instance : tile.mainLine) {
            for (const WorkoutGameMeshVertex &vertex : instance.mesh.vertices) {
                featureHalfWidth = std::max(
                        featureHalfWidth,
                        std::abs(vertex.rightMeters)
                            * std::max(instance.entryRightScale,
                                       instance.exitRightScale));
            }
        }
        double bypassCenter = 0.0;
        for (std::size_t index = 0;
             index + 3u < tile.bypass.mesh.vertices.size(); index += 4u) {
            bypassCenter = std::max(
                    bypassCenter,
                    std::abs((tile.bypass.mesh.vertices[index + 1].rightMeters
                              + tile.bypass.mesh.vertices[index + 2].rightMeters)
                             * 0.5));
        }
        QVERIFY2(bypassCenter - featureHalfWidth >= 0.55,
                 "bypass does not clear the feature and rider envelope");
    }

    void challengeTileKeepsAContinuousTrailSurfaceUnderAnObstacle()
    {
        const WorkoutGameRoadCourse course = featureCourse(
                WorkoutGameTerrainKind::LogOver, 0.7);
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());
        const WorkoutGameTrailTile tile =
                WorkoutGameTrailTileAssembler::challenge(course, *piece);
        QVERIFY(tile.ready);

        std::vector<std::pair<double, double>> dirtSpans;
        for (const WorkoutGameMeshInstance &instance : tile.mainLine) {
            const bool hasDirt = std::any_of(
                    instance.mesh.triangles.begin(),
                    instance.mesh.triangles.end(),
                    [](const WorkoutGameMeshTriangle &triangle) {
                        return triangle.material == WorkoutGameMeshMaterial::Dirt
                                || triangle.material
                                    == WorkoutGameMeshMaterial::DirtHighlight;
                    });
            if (!hasDirt) continue;
            QCOMPARE(instance.renderLayer,
                     WorkoutGameMeshRenderLayer::TrailSurface);
            QVERIFY(instance.clipToRoadOcclusion);
            dirtSpans.emplace_back(
                    instance.anchorDistanceMeters
                        + instance.mesh.entry.forwardMeters
                            * instance.forwardScale,
                    instance.anchorDistanceMeters
                        + instance.mesh.exit.forwardMeters
                            * instance.forwardScale);
        }
        std::sort(dirtSpans.begin(), dirtSpans.end());
        QVERIFY(!dirtSpans.empty());
        QVERIFY(dirtSpans.front().first <= tile.entryDistanceMeters + 1e-9);
        double coveredUntil = dirtSpans.front().second;
        for (std::size_t index = 1; index < dirtSpans.size(); ++index) {
            QVERIFY2(dirtSpans[index].first <= coveredUntil + 1e-9,
                     "feature tile leaves a hole through to the background");
            coveredUntil = std::max(coveredUntil, dirtSpans[index].second);
        }
        QVERIFY(coveredUntil >= tile.exitDistanceMeters - 1e-9);

        WorkoutGameRoadProjectionFrame hidden;
        hidden.ready = true;
        hidden.verticalExaggeration = 1.0;
        WorkoutGameRoadProjectedSlice far;
        far.worldDistanceMeters = tile.exitDistanceMeters + 1.0;
        far.depthMeters = 100.0;
        far.centerX = 640.0;
        far.centerY = 500.0;
        far.halfWidthPixels = 12.0;
        far.halfWidthMeters = 0.7;
        far.pixelsPerMeter = 4.0;
        far.occlusionY = 100.0;
        WorkoutGameRoadProjectedSlice near = far;
        near.worldDistanceMeters = tile.entryDistanceMeters - 1.0;
        near.depthMeters = 1.0;
        near.halfWidthPixels = 120.0;
        hidden.slices = {far, near};
        const auto underlay = std::find_if(
                tile.mainLine.begin(), tile.mainLine.end(),
                [](const WorkoutGameMeshInstance &instance) {
                    return instance.renderLayer
                            == WorkoutGameMeshRenderLayer::TrailSurface;
                });
        QVERIFY(underlay != tile.mainLine.end());
        QVERIFY2(WorkoutGameMeshProjector::project(*underlay, hidden).empty(),
                 "trail underlay renders through foreground terrain");
    }

    void occludedPropsAreClippedInsteadOfPoppingAsAWholeObject()
    {
        const std::array<WorkoutGameOcclusionVertex, 4> tree = {{
            {10.0, 130.0, 22.0, 100.0},
            {30.0, 130.0, 22.0, 100.0},
            {30.0, 70.0, 22.0, 100.0},
            {10.0, 70.0, 22.0, 100.0}
        }};
        const auto clipped = WorkoutGameOcclusion::clip(tree);
        QVERIFY(clipped.size() >= 4u);
        for (const WorkoutGameOcclusionVertex &vertex : clipped) {
            QVERIFY(vertex.y <= vertex.occlusionY + 1e-9);
        }
        const auto boundary = std::count_if(
                clipped.begin(), clipped.end(),
                [](const WorkoutGameOcclusionVertex &vertex) {
                    return std::abs(vertex.y - vertex.occlusionY) < 1e-9;
                });
        QCOMPARE(boundary, 2);

        const std::array<WorkoutGameOcclusionVertex, 3> invalid = {{
            {0.0, 0.0, 1.0, 1.0},
            {1.0, std::numeric_limits<double>::quiet_NaN(), 1.0, 1.0},
            {2.0, 0.0, 1.0, 1.0}
        }};
        QVERIFY(WorkoutGameOcclusion::clip(invalid).empty());

        const std::array<WorkoutGameOcclusionVertex, 4> boundaryVertex = {{
            {0.0, 100.0, 1.0, 100.0},
            {1.0, 120.0, 1.0, 100.0},
            {2.0, 80.0, 1.0, 100.0},
            {3.0, 80.0, 1.0, 100.0}
        }};
        const auto boundaryClipped =
                WorkoutGameOcclusion::clip(boundaryVertex);
        for (std::size_t index = 1; index < boundaryClipped.size(); ++index) {
            QVERIFY(std::abs(boundaryClipped[index - 1].x
                        - boundaryClipped[index].x) > 1e-9
                    || std::abs(boundaryClipped[index - 1].y
                        - boundaryClipped[index].y) > 1e-9);
        }
    }

    void logObstacleClearsButDoesNotDwarfTheSingletrack()
    {
        const WorkoutGameMesh log = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::LogOver, 0.7);
        QVERIFY(WorkoutGameMeshLibrary::valid(log));

        double halfWidth = 0.0;
        for (const WorkoutGameMeshVertex &vertex : log.vertices) {
            halfWidth = std::max(halfWidth, std::abs(vertex.rightMeters));
        }
        QVERIFY2(halfWidth >= 0.75,
                 "the log no longer reaches beyond both trail edges");
        QVERIFY2(halfWidth <= 1.0,
                 "the log becomes a wall in the near camera projection");
    }

    void tabletopHasCurvedTakeoffDeckAndLandingGeometry()
    {
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::Tabletop, 0.7);
        const WorkoutGameMesh tabletop = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::Tabletop, 0.7);
        QVERIFY(profile.ready);
        QCOMPARE(profile.shape,
                 WorkoutGameFeatureGeometryShape::CurvedTabletop);
        QVERIFY(WorkoutGameMeshLibrary::valid(tabletop));
        QVERIFY(tabletop.vertices.size() >= 80u);
        QVERIFY(tabletop.triangles.size() >= 80u);
        const double takeoffMiddle =
                (profile.startMeters + profile.plateauStartMeters) * 0.5;
        const double landingMiddle =
                (profile.plateauEndMeters + profile.endMeters) * 0.5;
        const double takeoffMiddleHeight =
                profile.surfaceOffset(takeoffMiddle);
        const double landingMiddleHeight =
                profile.surfaceOffset(landingMiddle);
        QVERIFY(takeoffMiddleHeight > profile.heightMeters * 0.35);
        QVERIFY(takeoffMiddleHeight < profile.heightMeters * 0.50);
        QCOMPARE(landingMiddleHeight, takeoffMiddleHeight);
        QCOMPARE(profile.surfaceOffset(profile.plateauStartMeters),
                 profile.heightMeters);
        QCOMPARE(profile.surfaceOffset(profile.plateauEndMeters),
                 profile.heightMeters);
    }

    void tabletopFitsTheGradeTwoReferenceEnvelope()
    {
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::Tabletop, 0.7);
        QVERIFY(profile.ready);
        const double takeoffRun = profile.plateauStartMeters
                - profile.startMeters;
        const double tabletopLength = profile.plateauEndMeters
                - profile.plateauStartMeters;
        const double landingRun = profile.endMeters
                - profile.plateauEndMeters;
        QVERIFY(profile.heightMeters <= 0.5 + 1e-9);
        QVERIFY(takeoffRun >= profile.heightMeters * 3.0);
        QVERIFY(takeoffRun <= 2.0);
        QVERIFY(tabletopLength >= 1.0);
        QVERIFY(tabletopLength <= 3.0);
        QVERIFY(landingRun >= profile.heightMeters * 3.0);
        QVERIFY(landingRun <= 2.0);
        QVERIFY(profile.endMeters - profile.startMeters <= 5.0);
        const WorkoutGameMesh tabletop = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::Tabletop, 0.7);
        const auto highlightCount = std::count_if(
                tabletop.triangles.begin(), tabletop.triangles.end(),
                [](const WorkoutGameMeshTriangle &triangle) {
                    return triangle.material
                            == WorkoutGameMeshMaterial::DirtHighlight;
                });
        QVERIFY(highlightCount >= 4);
        QVERIFY(highlightCount <= 8);

        const double linearStart = profile.startMeters + takeoffRun * 0.3;
        const double linearMiddle = profile.startMeters + takeoffRun * 0.6;
        const double linearEnd = profile.startMeters + takeoffRun * 0.9;
        const double firstSlope =
                (profile.surfaceOffset(linearMiddle)
                 - profile.surfaceOffset(linearStart))
                / (linearMiddle - linearStart);
        const double secondSlope =
                (profile.surfaceOffset(linearEnd)
                 - profile.surfaceOffset(linearMiddle))
                / (linearEnd - linearMiddle);
        QVERIFY(std::abs(firstSlope - secondSlope) < 1e-9);
        constexpr double Pi = 3.14159265358979323846;
        QVERIFY(std::atan(secondSlope) * 180.0 / Pi <= 15.0 + 1e-9);
    }

    void dropHasASharpLedgeAndLowerLandingInsteadOfADip()
    {
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::Drop, 0.65);
        QVERIFY(profile.ready);
        QCOMPARE(profile.shape,
                 WorkoutGameFeatureGeometryShape::DropLedge);
        QCOMPARE(profile.startMeters, -10.0);
        QCOMPARE(profile.plateauStartMeters, 0.0);
        QCOMPARE(profile.landingStartMeters, 1.25);
        QCOMPARE(profile.recoveryStartMeters, 5.0);
        QCOMPARE(profile.endMeters, 12.0);
        QVERIFY(profile.heightMeters <= -0.35);
        QVERIFY(profile.heightMeters >= -0.70);
        QCOMPARE(profile.surfaceOffset(-0.01), 0.0);
        QCOMPARE(profile.surfaceOffset(0.0), 0.0);
        QVERIFY(!profile.surfacePresent(0.01));
        QVERIFY(!profile.surfacePresent(1.24));
        QVERIFY(profile.surfacePresent(profile.landingStartMeters));
        QCOMPARE(profile.surfaceOffset(profile.landingStartMeters),
                 profile.heightMeters);
        QCOMPARE(profile.surfaceOffset(2.0), profile.heightMeters);
        QCOMPARE(profile.surfaceOffset(profile.endMeters), 0.0);

        const WorkoutGameMesh drop = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::Drop, 0.65);
        QVERIFY(WorkoutGameMeshLibrary::valid(drop));
        double lipTop = -std::numeric_limits<double>::infinity();
        double lipBottom = std::numeric_limits<double>::infinity();
        for (const WorkoutGameMeshVertex &vertex : drop.vertices) {
            if (std::abs(vertex.forwardMeters) > 1e-9) continue;
            lipTop = std::max(lipTop, vertex.upMeters);
            lipBottom = std::min(lipBottom, vertex.upMeters);
        }
        QVERIFY(std::abs(lipTop) < 1e-9);
        QVERIFY(lipBottom <= profile.heightMeters + 1e-9);
    }

    void logUsesAReadableRoundedCrossSection()
    {
        QVERIFY(WorkoutGameLogRadialSegments >= 16);
        const WorkoutGameMesh log = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::LogOver, 0.7);
        QVERIFY(WorkoutGameMeshLibrary::valid(log));
        QVERIFY(log.vertices.size()
                >= std::size_t(WorkoutGameLogRadialSegments * 2 + 2));
        QVERIFY(log.triangles.size()
                >= std::size_t(WorkoutGameLogRadialSegments * 3));
    }

    void transformedMeshProjectsAndSortsFarFacesFirst()
    {
        const WorkoutGameRoadCourse course = straightCourse();
        QVERIFY(course.ready);
        WorkoutGameRoadProjectionConfig config;
        config.viewportWidth = 1280.0;
        config.viewportHeight = 720.0;
        const WorkoutGameRoadProjectionFrame road =
                WorkoutGameRoadProjection::project(course, 20.0, config);
        QVERIFY(road.ready);

        WorkoutGameMeshInstance instance;
        instance.mesh = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::LogOver, 0.5);
        instance.anchorDistanceMeters = 42.0;
        instance.lateralMeters = 0.4;
        instance.yawDegrees = 18.0;
        instance.forwardScale = 1.2;
        instance.entryRightScale = 0.8;
        instance.exitRightScale = 0.8;
        instance.upScale = 1.4;
        const std::vector<WorkoutGameProjectedMeshTriangle> projected =
                WorkoutGameMeshProjector::project(instance, road);

        QVERIFY(projected.size() >= 4u);
        for (std::size_t index = 1; index < projected.size(); ++index) {
            QVERIFY(projected[index - 1].depthMeters
                    >= projected[index].depthMeters);
        }
    }

    void canonicalObstacleSurfaceMatchesVisibleMeshHeight()
    {
        const WorkoutGameTerrainKind terrains[] = {
            WorkoutGameTerrainKind::LogOver,
            WorkoutGameTerrainKind::Tabletop
        };
        constexpr double Difficulty = 0.6;
        for (WorkoutGameTerrainKind terrain : terrains) {
            const WorkoutGameRoadCourse course =
                    featureCourse(terrain, Difficulty);
            const auto piece = std::find_if(
                    course.pieces.begin(), course.pieces.end(),
                    [](const WorkoutGameRoadPiece &candidate) {
                        return candidate.challenge.enabled;
                    });
            QVERIFY(piece != course.pieces.end());
            const WorkoutGameRoadSample surface =
                    WorkoutGameRoadCourseBuilder::sample(
                        course, piece->challenge.obstacleDistanceMeters);
            const WorkoutGameMesh mesh =
                    WorkoutGameMeshLibrary::feature(terrain, Difficulty);
            QVERIFY(surface.ready);
            QVERIFY(WorkoutGameMeshLibrary::valid(mesh));
            double top = 0.0;
            for (const WorkoutGameMeshVertex &vertex : mesh.vertices) {
                top = std::max(top, vertex.upMeters);
            }
            QVERIFY(std::abs(surface.surfaceOffsetMeters - top) < 1e-9);
        }
    }

    void canonicalObstacleSurfaceUsesVisibleMeshBounds()
    {
        const WorkoutGameTerrainKind terrains[] = {
            WorkoutGameTerrainKind::LogOver,
            WorkoutGameTerrainKind::Tabletop,
            WorkoutGameTerrainKind::Drop
        };
        constexpr double Difficulty = 0.6;
        for (WorkoutGameTerrainKind terrain : terrains) {
            const WorkoutGameFeatureGeometryProfile profile =
                    WorkoutGameFeatureGeometry::profile(terrain, Difficulty);
            const WorkoutGameMesh mesh =
                    WorkoutGameMeshLibrary::feature(terrain, Difficulty);
            QVERIFY(profile.ready);
            QVERIFY(WorkoutGameMeshLibrary::valid(mesh));
            QCOMPARE(mesh.entry.forwardMeters, profile.startMeters);
            QCOMPARE(mesh.exit.forwardMeters, profile.endMeters);
            QCOMPARE(mesh.lengthMeters,
                     profile.endMeters - profile.startMeters);
            for (const WorkoutGameMeshVertex &vertex : mesh.vertices) {
                if (terrain == WorkoutGameTerrainKind::LogOver
                        && std::abs(vertex.rightMeters) > 0.1) {
                    continue;
                }
                double visibleSurface =
                        -std::numeric_limits<double>::infinity();
                for (const WorkoutGameMeshVertex &candidate : mesh.vertices) {
                    if (terrain == WorkoutGameTerrainKind::LogOver
                            && std::abs(candidate.rightMeters) > 0.1) {
                        continue;
                    }
                    if (std::abs(candidate.forwardMeters
                                 - vertex.forwardMeters) < 1e-9) {
                        visibleSurface = std::max(
                                visibleSurface, candidate.upMeters);
                    }
                }
                QVERIFY(std::abs(profile.surfaceOffset(vertex.forwardMeters)
                                 - visibleSurface) < 1e-9);
            }

            const WorkoutGameRoadCourse course =
                    featureCourse(terrain, Difficulty);
            const auto piece = std::find_if(
                    course.pieces.begin(), course.pieces.end(),
                    [](const WorkoutGameRoadPiece &candidate) {
                        return candidate.challenge.enabled;
                    });
            QVERIFY(piece != course.pieces.end());
            const double obstacle = piece->challenge.obstacleDistanceMeters;
            const auto surfaceAt = [&course, obstacle](double local) {
                return WorkoutGameRoadCourseBuilder::sample(
                        course, obstacle + local).surfaceOffsetMeters;
            };
            QVERIFY(std::abs(surfaceAt(profile.startMeters - 0.01)) < 1e-9);
            QVERIFY(std::abs(surfaceAt(profile.endMeters + 0.01)) < 1e-9);
            QVERIFY(std::abs(surfaceAt(
                    (profile.plateauStartMeters
                     + profile.plateauEndMeters) * 0.5)
                    - profile.heightMeters) < 1e-9);
        }
    }

    void logSurfaceUsesTheVisibleFacetedChords()
    {
        constexpr double Difficulty = 0.6;
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::LogOver, Difficulty);
        QVERIFY(profile.ready);
        constexpr double Pi = 3.14159265358979323846;
        const double radius = profile.heightMeters * 0.5;
        for (int segment = 0;
             segment < WorkoutGameLogRadialSegments / 2; ++segment) {
            const double fromAngle = Pi
                    - double(segment) * 2.0 * Pi
                        / double(WorkoutGameLogRadialSegments);
            const double toAngle = Pi
                    - double(segment + 1) * 2.0 * Pi
                        / double(WorkoutGameLogRadialSegments);
            const double fromX = std::cos(fromAngle) * radius;
            const double toX = std::cos(toAngle) * radius;
            const double fromY =
                    std::sin(fromAngle) * profile.heightMeters;
            const double toY =
                    std::sin(toAngle) * profile.heightMeters;
            QVERIFY(std::abs(
                    profile.surfaceOffset((fromX + toX) * 0.5)
                    - (fromY + toY) * 0.5) < 1e-12);
        }
    }

    void facetedLogJoinsTheTrailWithoutVerticalTeleport()
    {
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::LogOver, 0.6);
        QVERIFY(profile.ready);
        QCOMPARE(profile.surfaceOffset(profile.startMeters), 0.0);
        QCOMPARE(profile.surfaceOffset(profile.endMeters), 0.0);
        QCOMPARE(profile.surfaceOffset(0.0), profile.heightMeters);
        const double epsilon = 1.0e-5;
        const double entry = profile.surfaceOffset(
                profile.startMeters + epsilon);
        const double exit = profile.surfaceOffset(
                profile.endMeters - epsilon);
        QVERIFY(entry > 0.0);
        QVERIFY(exit > 0.0);
        QVERIFY(entry < 0.01);
        QVERIFY(exit < 0.01);
        QVERIFY(std::abs(entry - exit) < 1.0e-12);
    }

    void baseAnchoringDoesNotAddCanonicalObstacleHeightTwice()
    {
        const WorkoutGameRoadCourse course = featureCourse(
                WorkoutGameTerrainKind::LogOver, 0.5);
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());
        const double obstacle = piece->challenge.obstacleDistanceMeters;
        const WorkoutGameRoadProjectionFrame projection =
                WorkoutGameRoadProjection::project(course, obstacle - 10.0);
        QVERIFY(projection.ready);
        const WorkoutGameRoadProjectedPoint raised =
                WorkoutGameRoadProjection::projectPoint(
                    projection, obstacle, 0.0, 0.0);
        const WorkoutGameRoadProjectedPoint base =
                WorkoutGameRoadProjection::projectPoint(
                    projection, obstacle, 0.0, 0.0, true);
        QVERIFY(raised.ready);
        QVERIFY(base.ready);
        QVERIFY(base.y > raised.y);

        WorkoutGameMeshInstance instance;
        instance.mesh = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::LogOver, 0.5);
        instance.anchorDistanceMeters = obstacle;
        instance.anchorToBaseSurface = true;
        instance.occlusionAllowancePixels = 18.0;
        QVERIFY(!WorkoutGameMeshProjector::project(instance, projection).empty());
    }

    void meshTrianglesAreClippedAtTheNearPlaneInsteadOfPopping()
    {
        const WorkoutGameRoadCourse course = straightCourse();
        const WorkoutGameRoadProjectionFrame projection =
                WorkoutGameRoadProjection::project(course, 20.0);
        QVERIFY(projection.ready);
        WorkoutGameMeshInstance instance;
        instance.mesh = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::Tabletop, 0.5);
        instance.anchorDistanceMeters =
                projection.slices.back().worldDistanceMeters + 0.2;
        instance.clipToRoadOcclusion = false;
        const std::vector<WorkoutGameProjectedMeshTriangle> triangles =
                WorkoutGameMeshProjector::project(instance, projection);
        QVERIFY(!triangles.empty());
        for (const WorkoutGameProjectedMeshTriangle &triangle : triangles) {
            for (const WorkoutGameProjectedMeshVertex &vertex : triangle.vertices) {
                QVERIFY(vertex.depthMeters + 1e-9
                        >= projection.slices.back().depthMeters);
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameMesh)
#include "testWorkoutGameMesh.moc"
