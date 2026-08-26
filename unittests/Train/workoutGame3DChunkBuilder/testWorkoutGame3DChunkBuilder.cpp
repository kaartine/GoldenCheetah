/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DChunkBuilder.h"

#include <QElapsedTimer>
#include <QTest>

#include <atomic>
#include <memory>

namespace {

WorkoutGameRoadCourse straightCourse(double lengthMeters)
{
    WorkoutGameRoadCourse course;
    course.ready = true;
    course.seed = 1709u;
    course.totalLengthMeters = lengthMeters;
    WorkoutGameRoadPiece piece;
    piece.lengthMeters = lengthMeters;
    piece.entry.halfWidthMeters = 0.68;
    piece.exit.halfWidthMeters = 0.68;
    piece.exit.zMeters = lengthMeters;
    course.pieces.push_back(piece);
    return course;
}

bool waitForChunk(
        WorkoutGame3DChunkBuilder &builder,
        WorkoutGame3DChunk &chunk,
        int timeoutMs = 3000)
{
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < timeoutMs) {
        if (builder.takeLatest(chunk)) return true;
        QTest::qWait(2);
    }
    return false;
}

}

class TestWorkoutGame3DChunkBuilder : public QObject
{
    Q_OBJECT

private slots:
    void newestRequestWinsAndBothMailboxesStayCapacityOne()
    {
        WorkoutGame3DChunkBuilder builder;
        const auto course = std::make_shared<const WorkoutGameRoadCourse>(
                straightCourse(5000.0));
        for (int bucket = 0; bucket < 40; ++bucket) {
            const double start = double(bucket) * 10.0;
            builder.request(
                    course, start, start + 145.0, bucket, 77);
        }

        WorkoutGame3DChunk chunk;
        QVERIFY(waitForChunk(builder, chunk));
        QCOMPARE(chunk.bucket, 39);
        QCOMPARE(chunk.courseGeneration, std::uint64_t(77));
        QVERIFY(chunk.floorReady());
        QCOMPARE(builder.maximumPendingDepth(), std::size_t(1));
        QCOMPARE(builder.maximumResultDepth(), std::size_t(1));
        QVERIFY(builder.completedBuildCount() >= std::uint64_t(1));
    }

    void resultMailboxReplacesAnUnconsumedChunk()
    {
        WorkoutGame3DChunkBuilder builder;
        const auto course = std::make_shared<const WorkoutGameRoadCourse>(
                straightCourse(5000.0));
        builder.request(course, 0.0, 145.0, 1, 2);
        QTRY_VERIFY_WITH_TIMEOUT(builder.completedBuildCount() >= 1, 3000);
        builder.request(course, 300.0, 445.0, 2, 2);
        QTRY_VERIFY_WITH_TIMEOUT(builder.completedBuildCount() >= 2, 3000);

        WorkoutGame3DChunk chunk;
        QVERIFY(builder.takeLatest(chunk));
        QCOMPARE(chunk.bucket, 2);
        QVERIFY(!builder.takeLatest(chunk));
        QCOMPARE(builder.maximumResultDepth(), std::size_t(1));
    }

    void completionCallbackRunsAfterPublishingTheResult()
    {
        WorkoutGame3DChunkBuilder builder;
        std::atomic<int> callbackCount{0};
        builder.setCompletionCallback(
                [&callbackCount]() {
                    callbackCount.fetch_add(1, std::memory_order_relaxed);
                });
        builder.request(
                std::make_shared<const WorkoutGameRoadCourse>(
                    straightCourse(500.0)),
                0.0, 145.0, 4, 9);
        QTRY_COMPARE_WITH_TIMEOUT(
                callbackCount.load(std::memory_order_relaxed), 1, 3000);
        WorkoutGame3DChunk chunk;
        QVERIFY(builder.takeLatest(chunk));
        QCOMPARE(chunk.bucket, 4);
    }

    void meshPayloadIsBuiltWithoutOwningAQuick3DObject()
    {
        const WorkoutGameRoadCourse course = straightCourse(250.0);
        const WorkoutGame3DMeshData data =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestFloor,
                    course, 25.0, 170.0);
        QVERIFY(data.ready);
        QVERIFY(data.sampleCount > 100);
        QVERIFY(data.triangleCount() > 0);
        QVERIFY(data.triangleCount() < 30000);

        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        geometry.setMeshData(data);
        QVERIFY(geometry.ready());
        QCOMPARE(geometry.sampleCount(), data.sampleCount);
        QCOMPARE(geometry.triangleCount(), data.triangleCount());
        QCOMPARE(geometry.vertexData(), data.vertexData);
        QCOMPARE(geometry.indexData(), data.indexData);
    }

    void shutdownCancelsOutstandingWork()
    {
        WorkoutGame3DChunkBuilder builder;
        const auto course = std::make_shared<const WorkoutGameRoadCourse>(
                straightCourse(100000.0));
        builder.request(course, 0.0, 100000.0, 1, 1);
        builder.request(course, 100.0, 100000.0, 2, 1);
        builder.shutdown();
        WorkoutGame3DChunk chunk;
        QVERIFY(!builder.takeLatest(chunk));
    }

    void repeatedConstructionAndImmediateShutdownIsRaceFree()
    {
        const auto course = std::make_shared<const WorkoutGameRoadCourse>(
                straightCourse(500.0));
        for (int iteration = 0; iteration < 100; ++iteration) {
            WorkoutGame3DChunkBuilder builder;
            builder.request(course, 0.0, 145.0, iteration, 1);
        }
    }
};

QTEST_MAIN(TestWorkoutGame3DChunkBuilder)
#include "testWorkoutGame3DChunkBuilder.moc"
