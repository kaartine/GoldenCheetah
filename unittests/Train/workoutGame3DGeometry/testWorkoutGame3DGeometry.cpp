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
                floor.vertexData(), floor.stride(), 0, sizeof(float));
        QVERIFY(std::abs(floorLeftX) > std::abs(trailLeftX) + 12.0f);
        QVERIFY(floorY < trailY - 0.09f);
        QCOMPARE(floor.vertexData().size(),
                 floor.sampleCount() * 4 * floor.stride());
        QCOMPARE(floor.indexData().size(),
                 (floor.sampleCount() - 1) * 12 * int(sizeof(quint32)));
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
        const int middleVertex = (floor.sampleCount() / 2) * 4;
        const float middleY = vertexFloat(
                floor.vertexData(), floor.stride(), middleVertex,
                sizeof(float));
        QVERIFY(std::abs(middleY + 0.09f) < 0.001f);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGame3DGeometry)
#include "testWorkoutGame3DGeometry.moc"
