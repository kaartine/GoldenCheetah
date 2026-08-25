/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DGeometry.h"
#include "WorkoutGame3DTerrainProfile.h"
#include "WorkoutGameRootGeometry.h"
#include "WorkoutGameTrailBranch.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

WorkoutGameRoadCourse straightCourse(double length, double rise = 0.0)
{
    WorkoutGameRoadCourse course;
    course.ready = true;
    course.seed = 42;
    course.totalLengthMeters = length;
    WorkoutGameRoadPiece piece;
    piece.lengthMeters = length;
    piece.riseMeters = rise;
    piece.entry.halfWidthMeters = 0.68;
    piece.exit.halfWidthMeters = 0.68;
    piece.exit.zMeters = length;
    piece.exit.elevationMeters = rise;
    piece.exit.gradePercent = length > 0.0 ? rise / length * 100.0 : 0.0;
    course.pieces.push_back(piece);
    return course;
}

float vertexFloat(const QByteArray &data, int stride, int vertex, int offset)
{
    float value = 0.0f;
    std::memcpy(&value,
                data.constData() + vertex * stride + offset,
                sizeof(value));
    return value;
}

quint32 indexValue(const QByteArray &data, int index)
{
    quint32 value = 0;
    std::memcpy(&value, data.constData() + index * int(sizeof(value)),
                sizeof(value));
    return value;
}

WorkoutGameRoadCourse dropCourse()
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 611u;
    source.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::RecoveryDescent;
    section.terrain = WorkoutGameTerrainKind::Drop;
    section.durationMs = source.durationMs;
    section.targetWatts = 150.0;
    section.gradePercent = -4.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    source.sections = {section};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

WorkoutGameRoadCourse bermCourse()
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 407u;
    source.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::Trail;
    section.terrain = WorkoutGameTerrainKind::Berm;
    section.durationMs = source.durationMs;
    section.lengthMeters = 70.0;
    section.targetWatts = 180.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    source.sections = {section};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

WorkoutGameRoadCourse rootsCourse()
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 713u;
    source.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::Trail;
    section.terrain = WorkoutGameTerrainKind::Roots;
    section.durationMs = source.durationMs;
    section.lengthMeters = 70.0;
    section.targetWatts = 180.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    source.sections = {section};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

}

class TestWorkoutGame3DGeometry : public QObject
{
    Q_OBJECT

private slots:
    void rootsBuildAProceduralBuriedTubeNetwork()
    {
        const WorkoutGameRoadCourse course = rootsCourse();
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());
        const WorkoutGameRootGeometryProfile profile =
                WorkoutGameRootGeometry::profile(piece->difficulty);
        WorkoutGame3DGeometry roots(WorkoutGame3DGeometry::Layer::Roots);
        roots.setCourse(course);

        QVERIFY(roots.ready());
        QCOMPARE(roots.sampleCount(),
                 int(profile.segments.size()) * 5);
        QCOMPARE(roots.vertexData().size(),
                 roots.sampleCount() * 8 * roots.stride());
        QCOMPARE(roots.indexData().size(),
                 int(profile.segments.size()) * 4 * 8 * 6
                    * int(sizeof(quint32)));
        QVERIFY(roots.boundsMax().x() - roots.boundsMin().x() > 1.4f);
        QVERIFY(roots.boundsMax().y() > roots.boundsMin().y() + 0.07f);
        QVERIFY(roots.boundsMax().z() > roots.boundsMin().z() + 3.0f);
    }

    void rootsRangeBuildExcludesDistantTiles()
    {
        WorkoutGame3DGeometry roots(WorkoutGame3DGeometry::Layer::Roots);
        const WorkoutGameRoadCourse course = rootsCourse();
        roots.setCourseRange(course, 0.0, 5.0);
        QVERIFY(!roots.ready());
        QCOMPARE(roots.sampleCount(), 0);

        roots.setCourse(course);
        QVERIFY(roots.ready());
    }

    void invalidCourseClearsGeometry()
    {
        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(straightCourse(10.0));
        QVERIFY(geometry.ready());

        geometry.setCourse(WorkoutGameRoadCourse());
        QVERIFY(!geometry.ready());
        QVERIFY(geometry.vertexData().isEmpty());
        QVERIFY(geometry.indexData().isEmpty());
    }

    void trailHasStableIndexedVertexLayout()
    {
        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(straightCourse(12.0));

        QVERIFY(geometry.ready());
        QCOMPARE(geometry.stride(), 48);
        QCOMPARE(geometry.attributeCount(), 5);
        QCOMPARE(geometry.primitiveType(),
                 QQuick3DGeometry::PrimitiveType::Triangles);
        QCOMPARE(geometry.vertexData().size(),
                 geometry.sampleCount() * 2 * geometry.stride());
        QCOMPARE(geometry.indexData().size(),
                 (geometry.sampleCount() - 1) * 6 * int(sizeof(quint32)));
        QCOMPARE(geometry.attribute(0).semantic,
                 QQuick3DGeometry::Attribute::PositionSemantic);
        QCOMPARE(geometry.attribute(4).semantic,
                 QQuick3DGeometry::Attribute::IndexSemantic);
    }

    void trailPreservesAcceptedRiderScale()
    {
        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(straightCourse(12.0));

        QVERIFY(geometry.ready());
        const float leftX = vertexFloat(
                geometry.vertexData(), geometry.stride(), 0, 0);
        const float rightX = vertexFloat(
                geometry.vertexData(), geometry.stride(), 1, 0);
        QVERIFY(std::abs(leftX + 0.68f) < 0.001f);
        QVERIFY(std::abs(rightX - 0.68f) < 0.001f);
    }

    void floorIsWiderAndBelowTrail()
    {
        WorkoutGame3DGeometry trail(
                WorkoutGame3DGeometry::Layer::Trail);
        WorkoutGame3DGeometry floor(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        const WorkoutGameRoadCourse course = straightCourse(8.0);
        trail.setCourse(course);
        floor.setCourse(course);

        const float trailLeftX = vertexFloat(
                trail.vertexData(), trail.stride(), 0, 0);
        const float floorLeftX = vertexFloat(
                floor.vertexData(), floor.stride(), 0, 0);
        const float trailY = vertexFloat(
                trail.vertexData(), trail.stride(), 0, sizeof(float));
        const float floorY = vertexFloat(
                floor.vertexData(), floor.stride(), 3, sizeof(float));
        QVERIFY(std::abs(floorLeftX) > std::abs(trailLeftX) + 12.0f);
        QVERIFY(floorY < trailY - 0.02f);
        QCOMPARE(floor.vertexData().size(),
                 floor.sampleCount() * 8 * floor.stride());
        QCOMPARE(floor.indexData().size(),
                 (floor.sampleCount() - 1) * 42 * int(sizeof(quint32)));
    }

    void forestFloorShouldersJoinBothTrailEdges()
    {
        WorkoutGame3DGeometry trail(
                WorkoutGame3DGeometry::Layer::Trail);
        WorkoutGame3DGeometry floor(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        const WorkoutGameRoadCourse course = straightCourse(12.0);
        trail.setCourse(course);
        floor.setCourse(course);

        const float trailLeftX = vertexFloat(
                trail.vertexData(), trail.stride(), 0, 0);
        const float trailRightX = vertexFloat(
                trail.vertexData(), trail.stride(), 1, 0);
        const float trailY = vertexFloat(
                trail.vertexData(), trail.stride(), 0, sizeof(float));
        const float floorLeftX = vertexFloat(
                floor.vertexData(), floor.stride(), 3, 0);
        const float floorRightX = vertexFloat(
                floor.vertexData(), floor.stride(), 4, 0);
        const float floorLeftY = vertexFloat(
                floor.vertexData(), floor.stride(), 3, sizeof(float));
        const float floorRightY = vertexFloat(
                floor.vertexData(), floor.stride(), 4, sizeof(float));

        QVERIFY(std::abs(trailLeftX - floorLeftX) < 0.001f);
        QVERIFY(std::abs(trailRightX - floorRightX) < 0.001f);
        QVERIFY(std::abs(trailY - floorLeftY) < 0.04f);
        QVERIFY(std::abs(trailY - floorRightY) < 0.04f);
    }

    void bermMeshAndForestUseTheSameBankedEdgeSockets()
    {
        const WorkoutGameRoadCourse course = bermCourse();
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());
        const double center = piece->challenge.obstacleDistanceMeters;
        WorkoutGame3DGeometry trail(WorkoutGame3DGeometry::Layer::Trail);
        WorkoutGame3DGeometry berm(WorkoutGame3DGeometry::Layer::Berm);
        WorkoutGame3DGeometry floor(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        trail.setCourse(course);
        berm.setCourse(course);
        floor.setCourse(course);
        QVERIFY(trail.ready() && berm.ready() && floor.ready());

        int bermRow = -1;
        for (int row = 0; row < berm.sampleCount(); ++row) {
            const double distance = vertexFloat(
                    berm.vertexData(), berm.stride(), row * 7, 44) / 0.22;
            if (std::abs(distance - center) < 0.001) bermRow = row;
        }
        int floorRow = -1;
        for (int row = 0; row < floor.sampleCount(); ++row) {
            const double distance = vertexFloat(
                    floor.vertexData(), floor.stride(), row * 8, 44) / 0.22;
            if (std::abs(distance - center) < 0.001) floorRow = row;
        }
        QVERIFY(bermRow >= 0 && floorRow >= 0);
        const float trailLeftY = vertexFloat(
                berm.vertexData(), berm.stride(), bermRow * 7 + 1, 4);
        const float trailRightY = vertexFloat(
                berm.vertexData(), berm.stride(), bermRow * 7 + 5, 4);
        const float floorLeftY = vertexFloat(
                floor.vertexData(), floor.stride(), floorRow * 8 + 3, 4);
        const float floorRightY = vertexFloat(
                floor.vertexData(), floor.stride(), floorRow * 8 + 4, 4);
        QVERIFY(std::abs(trailLeftY - trailRightY) > 0.60f);
        QVERIFY(std::abs(trailLeftY - floorLeftY - 0.035f) < 0.002f);
        QVERIFY(std::abs(trailRightY - floorRightY - 0.035f) < 0.002f);

        const WorkoutGameRoadSample road =
                WorkoutGameRoadCourseBuilder::sample(course, center);
        QVERIFY(road.ready);
        QVERIFY(std::abs(road.bermBankRadians) > 0.30);
        QVERIFY((trailLeftY - trailRightY) * piece->turnRadians > 0.0);
        QCOMPARE(berm.vertexData().size(),
                 berm.sampleCount() * 7 * berm.stride());
        QVERIFY(berm.sampleCount() >= 52);
        QVERIFY(berm.indexData().size()
                / int(3 * sizeof(quint32)) <= 768);
        QVERIFY(trail.indexData().size()
                < (trail.sampleCount() - 1) * 6 * int(sizeof(quint32)));
        QVERIFY(floor.indexData().size()
                < (floor.sampleCount() - 1) * 42 * int(sizeof(quint32)));
    }

    void bermDoesNotCreateASeparateBypassRibbon()
    {
        WorkoutGame3DGeometry bypass(
                WorkoutGame3DGeometry::Layer::Bypass);
        bypass.setCourse(bermCourse());
        QVERIFY(!bypass.ready());
        QCOMPARE(bypass.sampleCount(), 0);
        QVERIFY(bypass.vertexData().isEmpty());
        QVERIFY(bypass.indexData().isEmpty());
    }

    void forestFloorHasReliefMaterialsAndUnitNormals()
    {
        WorkoutGame3DGeometry floor(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        floor.setCourse(straightCourse(24.0));
        QVERIFY(floor.ready());

        const int base = (floor.sampleCount() / 2) * 8;
        float minimumY = 1000.0f;
        float maximumY = -1000.0f;
        for (int vertex = 0; vertex < 8; ++vertex) {
            const float y = vertexFloat(
                    floor.vertexData(), floor.stride(), base + vertex,
                    sizeof(float));
            const float nx = vertexFloat(
                    floor.vertexData(), floor.stride(), base + vertex, 12);
            const float ny = vertexFloat(
                    floor.vertexData(), floor.stride(), base + vertex, 16);
            const float nz = vertexFloat(
                    floor.vertexData(), floor.stride(), base + vertex, 20);
            minimumY = std::min(minimumY, y);
            maximumY = std::max(maximumY, y);
            QVERIFY(ny > 0.0f);
            QVERIFY(std::abs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0f)
                    < 0.001f);
        }
        QVERIFY(maximumY - minimumY > 0.30f);
        float colorDistance = 0.0f;
        for (int component = 0; component < 3; ++component) {
            colorDistance += std::abs(vertexFloat(
                    floor.vertexData(), floor.stride(), base,
                    24 + component * int(sizeof(float))) - vertexFloat(
                    floor.vertexData(), floor.stride(), base + 3,
                    24 + component * int(sizeof(float))));
        }
        QVERIFY(colorDistance > 0.15f);
    }

    void risingCourseExpandsVerticalBounds()
    {
        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(straightCourse(50.0, 6.0));

        QVERIFY(geometry.ready());
        QVERIFY(geometry.boundsMax().y() > geometry.boundsMin().y() + 5.5f);
        QVERIFY(geometry.boundsMax().z() > 49.0f);
    }

    void longCourseHasBoundedSampleCount()
    {
        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(straightCourse(20000.0));

        QVERIFY(geometry.ready());
        QCOMPARE(geometry.sampleCount(), 16000);
        QVERIFY(geometry.vertexData().size() < 2 * 1024 * 1024);
        QVERIFY(geometry.indexData().size() < 512 * 1024);
    }

    void authoredObstacleDoesNotRaiseTheRenderedTrailBase()
    {
        WorkoutGameRoadCourse course = straightCourse(20000.0);
        WorkoutGameRoadPiece &piece = course.pieces.front();
        piece.terrain = WorkoutGameTerrainKind::LogOver;
        piece.difficulty = 1.0;
        piece.challenge.enabled = true;
        piece.challenge.obstacleDistanceMeters = 10000.37;

        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(course);

        QVERIFY(geometry.ready());
        QVERIFY(geometry.sampleCount() <= 16000);
        bool sampledObstacle = false;
        for (int sample = 0; sample < geometry.sampleCount(); ++sample) {
            const float y = vertexFloat(
                    geometry.vertexData(), geometry.stride(),
                    sample * 2, sizeof(float));
            const float z = vertexFloat(
                    geometry.vertexData(), geometry.stride(),
                    sample * 2, 2 * int(sizeof(float)));
            const WorkoutGameRoadSample road =
                    WorkoutGameRoadCourseBuilder::sample(course, z);
            QVERIFY(road.ready);
            QVERIFY(std::abs(y
                    - float(road.visualGroundElevationMeters() + 0.015))
                    < 0.001f);
            if (std::abs(z - piece.challenge.obstacleDistanceMeters)
                    < 0.001f) {
                sampledObstacle = true;
                QVERIFY(road.nonPhysicalFeatureOffsetMeters > 0.60);
                QVERIFY(y < road.center.elevationMeters - 0.50);
            }
        }
        QVERIFY(sampledObstacle);
    }

    void rangeBuildContainsOnlyRequestedCourseChunk()
    {
        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        geometry.setCourseRange(straightCourse(1000.0), 300.0, 520.0);

        QVERIFY(geometry.ready());
        QVERIFY(geometry.boundsMin().z() >= 299.9f);
        QVERIFY(geometry.boundsMax().z() <= 520.1f);
        QVERIFY(geometry.sampleCount() < 400);

        geometry.setCourseRange(straightCourse(1000.0), 520.0, 300.0);
        QVERIFY(!geometry.ready());
        QVERIFY(geometry.vertexData().isEmpty());
    }

    void forestFloorDoesNotCopyTrailReliefAcrossItsWidth()
    {
        WorkoutGame3DGeometry floor(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        floor.setCourse(straightCourse(24.0));

        QVERIFY(floor.ready());
        const int middleVertex = (floor.sampleCount() / 2) * 8;
        const float outerY = vertexFloat(
                floor.vertexData(), floor.stride(), middleVertex,
                sizeof(float));
        const float trailEdgeY = vertexFloat(
                floor.vertexData(), floor.stride(), middleVertex + 3,
                sizeof(float));
        QVERIFY(std::abs(outerY - trailEdgeY) > 0.10f);
    }

    void dropTrailIndicesDoNotBridgeTheAirGap()
    {
        const WorkoutGameRoadCourse course = dropCourse();
        QVERIFY(course.ready);
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());
        const double lip = piece->challenge.obstacleDistanceMeters;
        WorkoutGame3DGeometry trail(WorkoutGame3DGeometry::Layer::Trail);
        trail.setCourse(course);
        QVERIFY(trail.ready());

        const int indexCount = trail.indexData().size()
                / int(sizeof(quint32));
        QVERIFY(indexCount
                < (trail.sampleCount() - 1) * 6);
        for (int index = 0; index + 2 < indexCount; index += 3) {
            double minimumDistance = std::numeric_limits<double>::infinity();
            double maximumDistance =
                    -std::numeric_limits<double>::infinity();
            for (int corner = 0; corner < 3; ++corner) {
                const int vertex = int(indexValue(
                        trail.indexData(), index + corner));
                const double distance = vertexFloat(
                        trail.vertexData(), trail.stride(), vertex, 44) / 0.22;
                minimumDistance = std::min(minimumDistance, distance);
                maximumDistance = std::max(maximumDistance, distance);
            }
            QVERIFY2(!(minimumDistance <= lip + 1e-4
                       && maximumDistance >= lip + 1.25 - 1e-4),
                     "trail triangle bridges the drop air gap");
        }
    }

    void bypassUsesTheRuntimeBranchCurveAndTerrainSurface()
    {
        WorkoutGameRoadCourse course = straightCourse(40.0);
        WorkoutGameRoadPiece &piece = course.pieces.front();
        piece.challenge.enabled = true;
        piece.challenge.obstacleDistanceMeters = 20.0;
        piece.challenge.bypassStartDistanceMeters = 10.0;
        piece.challenge.bypassEndDistanceMeters = 30.0;
        piece.challenge.bypassLateralMeters = 2.2;

        WorkoutGame3DGeometry bypass(
                WorkoutGame3DGeometry::Layer::Bypass);
        bypass.setCourse(course);

        QVERIFY(bypass.ready());
        QVERIFY(bypass.sampleCount() >= 20);
        QCOMPARE(bypass.vertexData().size(),
                 bypass.sampleCount() * 4 * bypass.stride());
        QCOMPARE(bypass.indexData().size(),
                 (bypass.sampleCount() - 1) * 18
                    * int(sizeof(quint32)));
        double maximumCenter = 0.0;
        for (int row = 0; row < bypass.sampleCount(); ++row) {
            const int base = row * 4;
            const double distance = vertexFloat(
                    bypass.vertexData(), bypass.stride(), base, 8);
            const double left = vertexFloat(
                    bypass.vertexData(), bypass.stride(), base + 1, 0);
            const double right = vertexFloat(
                    bypass.vertexData(), bypass.stride(), base + 2, 0);
            const double center = (left + right) * 0.5;
            const double expectedCenter = WorkoutGameTrailBranch::lateralAt(
                    distance,
                    piece.challenge.bypassStartDistanceMeters,
                    piece.challenge.bypassEndDistanceMeters,
                    piece.challenge.bypassLateralMeters);
            QVERIFY(std::abs(center - expectedCenter) < 0.001);
            maximumCenter = std::max(maximumCenter, std::abs(center));

            const WorkoutGameRoadSample road =
                    WorkoutGameRoadCourseBuilder::sample(course, distance);
            const WorkoutGame3DTerrainProfileSnapshot profile =
                    WorkoutGame3DTerrainProfile::build(
                        road, distance, course.seed);
            QVERIFY(road.ready && profile.ready);
            const double branchBlend = WorkoutGameTrailBranch::blend(
                    (distance
                     - piece.challenge.bypassStartDistanceMeters)
                    / (piece.challenge.bypassEndDistanceMeters
                       - piece.challenge.bypassStartDistanceMeters));
            for (int vertex = 0; vertex < 4; ++vertex) {
                const double lateral = vertexFloat(
                        bypass.vertexData(), bypass.stride(), base + vertex, 0);
                const double y = vertexFloat(
                        bypass.vertexData(), bypass.stride(), base + vertex, 4);
                const double expected =
                        WorkoutGame3DTerrainProfile::elevationAtLateral(
                            profile, lateral)
                        + (vertex == 1 || vertex == 2
                            ? WorkoutGameTrailBranch::treadLiftMeters(
                                branchBlend)
                            : WorkoutGameTrailBranch::edgeLiftMeters(
                                branchBlend));
                QVERIFY(std::abs(y - expected) < 0.001);
                const double nx = vertexFloat(
                        bypass.vertexData(), bypass.stride(), base + vertex, 12);
                const double ny = vertexFloat(
                        bypass.vertexData(), bypass.stride(), base + vertex, 16);
                const double nz = vertexFloat(
                        bypass.vertexData(), bypass.stride(), base + vertex, 20);
                QVERIFY(ny > 0.0);
                QVERIFY(std::abs(std::sqrt(nx * nx + ny * ny + nz * nz)
                                 - 1.0) < 0.001);
            }
        }
        QVERIFY(maximumCenter > 2.1);
        const double firstOuter = vertexFloat(
                bypass.vertexData(), bypass.stride(), 0, 0);
        const double lastOuter = vertexFloat(
                bypass.vertexData(), bypass.stride(),
                (bypass.sampleCount() - 1) * 4 + 3, 0);
        QVERIFY(std::abs(firstOuter + 0.68) < 0.001);
        QVERIFY(std::abs(lastOuter - 0.68) < 0.001);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGame3DGeometry)
#include "testWorkoutGame3DGeometry.moc"
