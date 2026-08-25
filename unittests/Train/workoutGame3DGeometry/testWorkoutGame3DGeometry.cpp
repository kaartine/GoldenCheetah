/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DGeometry.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstring>

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

}

class TestWorkoutGame3DGeometry : public QObject
{
    Q_OBJECT

private slots:
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

    void narrowFeatureApexIsAlwaysSampled()
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
        float maximumY = -1000.0f;
        for (int sample = 0; sample < geometry.sampleCount(); ++sample) {
            maximumY = std::max(maximumY, vertexFloat(
                    geometry.vertexData(), geometry.stride(),
                    sample * 2, sizeof(float)));
        }
        QVERIFY2(maximumY > 0.60f,
                 "narrow log apex was lost between uniform samples");
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
};

QTEST_GUILESS_MAIN(TestWorkoutGame3DGeometry)
#include "testWorkoutGame3DGeometry.moc"
