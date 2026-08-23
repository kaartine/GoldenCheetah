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

#include <QTest>

#include <algorithm>
#include <cmath>
#include <limits>

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
    void featureModelsAreValidAndReadyForTexturesAndCollision()
    {
        const WorkoutGameTerrainKind terrains[] = {
            WorkoutGameTerrainKind::LogOver,
            WorkoutGameTerrainKind::Roots,
            WorkoutGameTerrainKind::RockGarden,
            WorkoutGameTerrainKind::Tabletop,
            WorkoutGameTerrainKind::Drop
        };
        for (WorkoutGameTerrainKind terrain : terrains) {
            const WorkoutGameMesh model =
                    WorkoutGameMeshLibrary::feature(terrain, 0.7);
            QVERIFY2(WorkoutGameMeshLibrary::valid(model),
                     "feature mesh must have finite indexed triangles");
            QVERIFY(model.vertices.size() >= 8u);
            QVERIFY(model.triangles.size() >= 4u);
            QVERIFY(!model.colliders.empty());
            for (const WorkoutGameMeshVertex &vertex : model.vertices) {
                QVERIFY(std::isfinite(vertex.u));
                QVERIFY(std::isfinite(vertex.v));
            }
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
                20.0, 2.2, 0.55);
        QVERIFY(WorkoutGameMeshLibrary::valid(bypass));
        QVERIFY(bypass.vertices.size() >= 20u);

        const double startRight = bypass.vertices.front().rightMeters
                + bypass.vertices[1].rightMeters;
        const double endRight = bypass.vertices[bypass.vertices.size() - 2].rightMeters
                + bypass.vertices.back().rightMeters;
        double largestCenter = 0.0;
        for (std::size_t index = 0; index + 1 < bypass.vertices.size(); index += 2) {
            largestCenter = std::max(largestCenter, std::abs(
                    (bypass.vertices[index].rightMeters
                     + bypass.vertices[index + 1].rightMeters) * 0.5));
        }
        QVERIFY(std::abs(startRight) < 1e-9);
        QVERIFY(std::abs(endRight) < 1e-9);
        QVERIFY(largestCenter > 2.0);
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
        QVERIFY(profile.surfaceOffset(takeoffMiddle)
                < profile.heightMeters * 0.45);
        QVERIFY(profile.surfaceOffset(landingMiddle)
                > profile.heightMeters * 0.55);
        QCOMPARE(profile.surfaceOffset(profile.plateauStartMeters),
                 profile.heightMeters);
        QCOMPARE(profile.surfaceOffset(profile.plateauEndMeters),
                 profile.heightMeters);
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
        instance.rightScale = 0.8;
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
            WorkoutGameTerrainKind::Tabletop,
            WorkoutGameTerrainKind::RockSlab
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
            WorkoutGameTerrainKind::RockSlab,
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
                double visibleSurface =
                        -std::numeric_limits<double>::infinity();
                for (const WorkoutGameMeshVertex &candidate : mesh.vertices) {
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
            const double fromY = radius + std::sin(fromAngle) * radius;
            const double toY = radius + std::sin(toAngle) * radius;
            QVERIFY(std::abs(
                    profile.surfaceOffset((fromX + toX) * 0.5)
                    - (fromY + toY) * 0.5) < 1e-12);
        }
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
