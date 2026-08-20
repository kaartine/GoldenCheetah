/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameMesh.h"

#include <QTest>

#include <algorithm>
#include <cmath>

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
};

QTEST_GUILESS_MAIN(TestWorkoutGameMesh)
#include "testWorkoutGameMesh.moc"
