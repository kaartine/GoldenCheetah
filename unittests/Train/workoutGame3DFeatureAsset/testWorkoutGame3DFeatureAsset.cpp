/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGame3DFeatureAsset.h"
#include "Train/WorkoutGameAssetPhysicsSampler.h"
#include "Train/WorkoutGameFeatureGeometry.h"
#include "Train/WorkoutGameMesh.h"

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
    if (terrain == WorkoutGameTerrainKind::GapJump) {
        feature.lengthMeters = 120.0;
    }
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

std::size_t challengePieceIndex(const WorkoutGameRoadCourse &course)
{
    return std::size_t(&challengePiece(course) - course.pieces.data());
}

}

class TestWorkoutGame3DFeatureAsset : public QObject
{
    Q_OBJECT

private slots:
    void placesResolvedLogAssetFromPersistedRenderFit()
    {
        WorkoutGameRoadCourse course = courseWith(
                WorkoutGameTerrainKind::LogOver, 0.6);
        const std::size_t pieceIndex = challengePieceIndex(course);
        const WorkoutGameRoadPiece &piece = course.pieces[pieceIndex];

        auto snapshot = std::make_shared<
                WorkoutGameCourseAssetPhysicsSnapshot>();
        snapshot->catalogSchemaVersion = 1;
        snapshot->physicsDefinitions.resize(1);
        WorkoutGameAssetPhysicsBinding binding;
        binding.assetId = QStringLiteral("FT-02-log-over-greybox");
        binding.definitionIndex = 0;
        binding.nativeForwardOriginMm = -1020;
        binding.nativeForwardExtentMm = 540;
        binding.nativeUpExtentMm = 540;
        binding.resolvedExtentMm = 700;
        snapshot->bindings.push_back(binding);
        snapshot->pieceBindings.resize(course.pieces.size());
        auto &pieceBinding = snapshot->pieceBindings[pieceIndex];
        pieceBinding.definitionIndex = 0;
        pieceBinding.bindingIndex = 0;
        pieceBinding.obstacleAnchorMm = std::int32_t(std::llround(
                piece.challenge.obstacleDistanceMeters * 1000.0));
        pieceBinding.obstacleAnchorMicrometerRemainder = 490;
        course.assetPhysicsSnapshot = snapshot;

        const WorkoutGame3DFeatureAssetSnapshot asset =
                WorkoutGame3DFeatureAsset::placeAt(course, pieceIndex);
        QVERIFY(asset.ready);
        QCOMPARE(asset.terrain, WorkoutGameTerrainKind::LogOver);
        QCOMPARE(asset.scaleZ, 700.0 / 540.0);
        QCOMPARE(asset.scaleY, 700.0 / 540.0);

        const double expectedDistance =
                pieceBinding.obstacleAnchorMeters()
                - 1.02 * asset.scaleZ;
        const WorkoutGameRoadSample road =
                WorkoutGameRoadCourseBuilder::sample(course, expectedDistance);
        QVERIFY(road.ready);
        QVERIFY(std::abs(asset.xMeters - road.center.xMeters) < 1e-12);
        QVERIFY(std::abs(asset.yMeters
                         - road.visualGroundElevationMeters()) < 1e-12);
        QVERIFY(std::abs(asset.zMeters - road.center.zMeters) < 1e-12);

        snapshot->pieceBindings[pieceIndex].bindingIndex = 99;
        QVERIFY(!WorkoutGame3DFeatureAsset::placeAt(course, pieceIndex).ready);
    }

    void packagedAndFallbackLogMeshesShareThePhysicalObstacleBounds()
    {
        WorkoutGameRoadCourse course = courseWith(
                WorkoutGameTerrainKind::LogOver, 0.6);
        const std::size_t pieceIndex = challengePieceIndex(course);
        const WorkoutGameRoadPiece &piece = course.pieces[pieceIndex];
        auto snapshot = std::make_shared<
                WorkoutGameCourseAssetPhysicsSnapshot>();
        snapshot->catalogSchemaVersion = 1;
        snapshot->physicsDefinitions.resize(1);
        WorkoutGameAssetPhysicsBinding binding;
        binding.assetId = QStringLiteral("FT-02-log-over-greybox");
        binding.definitionIndex = 0;
        binding.nativeForwardOriginMm = -1020;
        binding.nativeForwardExtentMm = 540;
        binding.nativeUpExtentMm = 540;
        binding.resolvedExtentMm = 700;
        snapshot->bindings.push_back(binding);
        snapshot->pieceBindings.resize(course.pieces.size());
        auto &pieceBinding = snapshot->pieceBindings[pieceIndex];
        pieceBinding.definitionIndex = 0;
        pieceBinding.bindingIndex = 0;
        pieceBinding.obstacleAnchorMm = std::int32_t(std::llround(
                piece.challenge.obstacleDistanceMeters * 1000.0));
        course.assetPhysicsSnapshot = snapshot;

        const WorkoutGameAssetRenderTransform transform =
                WorkoutGameAssetPhysicsSampler::renderTransform(
                    *snapshot, pieceIndex);
        QCOMPARE(transform.status, WorkoutGameAssetRenderFitStatus::Ready);
        constexpr double packagedObstacleStartMeters = 0.75;
        constexpr double packagedObstacleEndMeters = 1.29;
        const double packagedStart = transform.assetStartDistanceMeters
                + packagedObstacleStartMeters * transform.forwardScale;
        const double packagedEnd = transform.assetStartDistanceMeters
                + packagedObstacleEndMeters * transform.forwardScale;

        const WorkoutGameMesh fallback = WorkoutGameMeshLibrary::feature(
                WorkoutGameTerrainKind::LogOver, piece.difficulty);
        QVERIFY(WorkoutGameMeshLibrary::valid(fallback));
        const double fallbackScale = transform.forwardExtentMeters
                / (fallback.exit.forwardMeters
                   - fallback.entry.forwardMeters);
        const double fallbackStart = transform.obstacleAnchorMeters
                + fallback.entry.forwardMeters * fallbackScale;
        const double fallbackEnd = transform.obstacleAnchorMeters
                + fallback.exit.forwardMeters * fallbackScale;
        QVERIFY(std::abs(packagedStart - fallbackStart) < 1e-12);
        QVERIFY(std::abs(packagedEnd - fallbackEnd) < 1e-12);
        double fallbackMinimum = fallback.vertices.front().forwardMeters;
        double fallbackMaximum = fallbackMinimum;
        double fallbackHeight = fallback.vertices.front().upMeters;
        for (const WorkoutGameMeshVertex &vertex : fallback.vertices) {
            fallbackMinimum = std::min(
                    fallbackMinimum, vertex.forwardMeters);
            fallbackMaximum = std::max(
                    fallbackMaximum, vertex.forwardMeters);
            fallbackHeight = std::max(fallbackHeight, vertex.upMeters);
        }
        QVERIFY(std::abs(packagedStart
                         - (transform.obstacleAnchorMeters
                            + fallbackMinimum * fallbackScale)) < 1e-12);
        QVERIFY(std::abs(packagedEnd
                         - (transform.obstacleAnchorMeters
                            + fallbackMaximum * fallbackScale)) < 1e-12);
        QVERIFY(fallbackHeight > 0.0);
        const double fallbackUpScale = transform.upExtentMeters
                / fallbackHeight;
        QVERIFY(std::abs(0.54 * transform.upScale
                         - fallbackHeight * fallbackUpScale)
                < 1e-12);
    }

    void rejectsUnsupportedOrUnavailableFeatures()
    {
        QVERIFY(!WorkoutGame3DFeatureAsset::place({}, {}).ready);
        const WorkoutGameRoadCourse course = courseWith(
                WorkoutGameTerrainKind::Roots, 0.5);
        QVERIFY(course.ready);
        QVERIFY(!WorkoutGame3DFeatureAsset::place(
                     course, challengePiece(course)).ready);
    }

    void leavesTabletopSurfaceToTheProceduralRoadGeometry()
    {
        const WorkoutGameRoadCourse course = courseWith(
                WorkoutGameTerrainKind::Tabletop, 0.7);
        const WorkoutGameRoadPiece &piece = challengePiece(course);
        const WorkoutGame3DFeatureAssetSnapshot asset =
                WorkoutGame3DFeatureAsset::place(course, piece);
        QVERIFY(!asset.ready);
    }

    void placesGapJumpAssetOnItsMeasuredCourseSockets()
    {
        const WorkoutGameRoadCourse course = courseWith(
                WorkoutGameTerrainKind::GapJump, 0.5);
        const WorkoutGameRoadPiece &piece = challengePiece(course);
        QVERIFY(piece.gapJump.enabled);
        const WorkoutGame3DFeatureAssetSnapshot asset =
                WorkoutGame3DFeatureAsset::place(course, piece);
        QVERIFY(asset.ready);
        QCOMPARE(asset.terrain, WorkoutGameTerrainKind::GapJump);
        QCOMPARE(asset.scaleY, 1.0);
        QCOMPARE(asset.scaleZ, 1.0);

        const WorkoutGameRoadSample socket =
                WorkoutGameRoadCourseBuilder::sample(
                    course, piece.gapJump.splitStartDistanceMeters);
        QVERIFY(socket.ready);
        QVERIFY(std::abs(asset.xMeters - socket.center.xMeters) < 1e-12);
        QVERIFY(std::abs(asset.yMeters
                         - socket.visualGroundElevationMeters()) < 1e-12);
        QVERIFY(std::abs(asset.zMeters - socket.center.zMeters) < 1e-12);
        QVERIFY(std::abs(asset.yawDegrees
                         - socket.center.headingRadians * 180.0
                            / 3.14159265358979323846) < 1e-12);

        QCOMPARE(piece.gapJump.lines[0].takeoffDistanceMeters
                    - piece.gapJump.splitStartDistanceMeters, 12.0);
        QCOMPARE(piece.gapJump.lines[0].landingDistanceMeters
                    - piece.gapJump.splitStartDistanceMeters, 13.8);
        QCOMPARE(piece.gapJump.lines[1].landingDistanceMeters
                    - piece.gapJump.splitStartDistanceMeters, 15.2);
        QCOMPARE(piece.gapJump.lines[2].landingDistanceMeters
                    - piece.gapJump.splitStartDistanceMeters, 16.7);
        QCOMPARE(piece.gapJump.mergeEndDistanceMeters
                    - piece.gapJump.splitStartDistanceMeters, 40.7);
    }

    void keepsGapAssetGeometryCanonicalWhileDifficultyChangesAuthority()
    {
        const WorkoutGameRoadCourse easy = courseWith(
                WorkoutGameTerrainKind::GapJump, 0.0);
        const WorkoutGameRoadCourse hard = courseWith(
                WorkoutGameTerrainKind::GapJump, 1.0);
        const WorkoutGameRoadPiece &easyPiece = challengePiece(easy);
        const WorkoutGameRoadPiece &hardPiece = challengePiece(hard);
        const WorkoutGame3DFeatureAssetSnapshot easyAsset =
                WorkoutGame3DFeatureAsset::place(easy, easyPiece);
        const WorkoutGame3DFeatureAssetSnapshot hardAsset =
                WorkoutGame3DFeatureAsset::place(hard, hardPiece);
        QVERIFY(easyAsset.ready);
        QVERIFY(hardAsset.ready);
        QCOMPARE(easyAsset.scaleY, 1.0);
        QCOMPARE(easyAsset.scaleZ, 1.0);
        QCOMPARE(hardAsset.scaleY, 1.0);
        QCOMPARE(hardAsset.scaleZ, 1.0);
        QCOMPARE(easyPiece.gapJump.lines[2].gapLengthMeters, 4.7);
        QCOMPARE(hardPiece.gapJump.lines[2].gapLengthMeters, 4.7);
        QVERIFY(hardPiece.gapJump.lines[2].minimumSpeedMetersPerSecond
                > easyPiece.gapJump.lines[2].minimumSpeedMetersPerSecond);
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

    void placesDropTileFromTheUpperLipSocket()
    {
        const WorkoutGameRoadCourse course = courseWith(
                WorkoutGameTerrainKind::Drop, 0.6);
        const WorkoutGameRoadPiece &piece = challengePiece(course);
        const WorkoutGame3DFeatureAssetSnapshot asset =
                WorkoutGame3DFeatureAsset::place(course, piece);
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece.terrain, piece.difficulty);

        QVERIFY(asset.ready);
        QCOMPARE(asset.terrain, WorkoutGameTerrainKind::Drop);
        QVERIFY(std::abs(asset.scaleY
                         - std::abs(profile.heightMeters) / 0.70) < 1e-12);
        QCOMPARE(asset.scaleZ, 1.0);

        const double expectedStart =
                piece.challenge.obstacleDistanceMeters + profile.startMeters;
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
