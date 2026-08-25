/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGame3DFeatureAsset.h"
#include "Train/WorkoutGameFeatureGeometry.h"

#include <QTest>

#include <algorithm>
#include <cmath>

namespace {

WorkoutGameRoadCourse courseWith(WorkoutGameTerrainKind terrain, double difficulty)
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 741u;
    source.durationMs = 60000;
    WorkoutGameSection approach;
    approach.feature = WorkoutGameFeature::Trail;
    approach.terrain = WorkoutGameTerrainKind::SmoothTrail;
    approach.durationMs = 15000;
    approach.targetWatts = 170.0;
    WorkoutGameSection feature = approach;
    feature.feature = WorkoutGameFeature::SprintJump;
    feature.terrain = terrain;
    feature.startMs = 15000;
    feature.durationMs = 30000;
    feature.targetWatts = 250.0;
    feature.difficulty = difficulty;
    feature.challengeCount = 1;
    WorkoutGameSection exit = approach;
    exit.startMs = 45000;
    source.sections = {approach, feature, exit};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

const WorkoutGameRoadPiece &challengePiece(const WorkoutGameRoadCourse &course)
{
    const auto result = std::find_if(
            course.pieces.begin(), course.pieces.end(),
            [](const WorkoutGameRoadPiece &piece) {
                return piece.challenge.enabled;
            });
    Q_ASSERT(result != course.pieces.end());
    return *result;
}

}

class TestWorkoutGame3DFeatureAsset : public QObject
{
    Q_OBJECT

private slots:
    void rejectsUnsupportedOrUnavailableFeatures()
    {
        QVERIFY(!WorkoutGame3DFeatureAsset::place({}, {}).ready);
        const WorkoutGameRoadCourse course = courseWith(
                WorkoutGameTerrainKind::Roots, 0.5);
        QVERIFY(course.ready);
        QVERIFY(!WorkoutGame3DFeatureAsset::place(
                     course, challengePiece(course)).ready);
    }

    void placesTabletopFromItsAuthoritativeProfile()
    {
        const WorkoutGameRoadCourse course = courseWith(
                WorkoutGameTerrainKind::Tabletop, 0.7);
        const WorkoutGameRoadPiece &piece = challengePiece(course);
        const WorkoutGame3DFeatureAssetSnapshot asset =
                WorkoutGame3DFeatureAsset::place(course, piece);
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece.terrain, piece.difficulty);

        QVERIFY(asset.ready);
        QCOMPARE(asset.terrain, WorkoutGameTerrainKind::Tabletop);
        QVERIFY(std::abs(asset.scaleY - profile.heightMeters / 0.446) < 1e-12);
        QVERIFY(std::abs(asset.scaleZ
                         - (profile.endMeters - profile.startMeters) / 4.84)
                < 1e-12);
    }

    void placesLogSocketTileAroundThePhysicalObstacle()
    {
        const WorkoutGameRoadCourse course = courseWith(
                WorkoutGameTerrainKind::LogOver, 0.6);
        const WorkoutGameRoadPiece &piece = challengePiece(course);
        const WorkoutGame3DFeatureAssetSnapshot asset =
                WorkoutGame3DFeatureAsset::place(course, piece);
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece.terrain, piece.difficulty);
        QVERIFY(asset.ready);
        QCOMPARE(asset.terrain, WorkoutGameTerrainKind::LogOver);
        const double expectedScale = profile.heightMeters / 0.54;
        QVERIFY(std::abs(asset.scaleY - expectedScale) < 1e-12);
        QVERIFY(std::abs(asset.scaleZ - expectedScale) < 1e-12);

        const double expectedStart =
                piece.challenge.obstacleDistanceMeters + profile.startMeters
                    - 0.75 * expectedScale;
        const WorkoutGameRoadSample road = WorkoutGameRoadCourseBuilder::sample(
                course, expectedStart);
        QVERIFY(road.ready);
        QVERIFY(std::abs(asset.xMeters - road.center.xMeters) < 1e-12);
        QVERIFY(std::abs(asset.yMeters - road.center.elevationMeters) < 1e-12);
        QVERIFY(std::abs(asset.zMeters - road.center.zMeters) < 1e-12);
    }

    void placesBunnyHopTileWithoutDistortingItsSocketLength()
    {
        const WorkoutGameRoadCourse course = courseWith(
                WorkoutGameTerrainKind::BunnyHop, 0.65);
        const WorkoutGameRoadPiece &piece = challengePiece(course);
        const WorkoutGame3DFeatureAssetSnapshot asset =
                WorkoutGame3DFeatureAsset::place(course, piece);
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece.terrain, piece.difficulty);
        QVERIFY(asset.ready);
        QCOMPARE(asset.terrain, WorkoutGameTerrainKind::BunnyHop);
        QVERIFY(std::abs(asset.scaleY - profile.heightMeters / 0.20) < 1e-12);
        QCOMPARE(asset.scaleZ, 1.0);

        const double expectedStart =
                piece.challenge.obstacleDistanceMeters
                + profile.startMeters - 1.68;
        const WorkoutGameRoadSample road = WorkoutGameRoadCourseBuilder::sample(
                course, expectedStart);
        QVERIFY(road.ready);
        QVERIFY(std::abs(asset.xMeters - road.center.xMeters) < 1e-12);
        QVERIFY(std::abs(asset.yMeters
                         - road.visualGroundElevationMeters()) < 1e-12);
        QVERIFY(std::abs(asset.zMeters - road.center.zMeters) < 1e-12);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGame3DFeatureAsset)
#include "testWorkoutGame3DFeatureAsset.moc"
