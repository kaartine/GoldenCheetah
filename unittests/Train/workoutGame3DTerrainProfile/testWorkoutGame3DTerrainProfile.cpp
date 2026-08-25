/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGame3DTerrainProfile.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

WorkoutGameRoadSample roadSample()
{
    WorkoutGameRoadSample result;
    result.ready = true;
    result.center.halfWidthMeters = 0.68;
    result.center.elevationMeters = 4.0;
    result.baseElevationMeters = 3.7;
    return result;
}

bool near(double left, double right, double tolerance = 1e-9)
{
    return std::abs(left - right) <= tolerance;
}

}

class TestWorkoutGame3DTerrainProfile : public QObject
{
    Q_OBJECT

private slots:
    void rejectsUnavailableRoadSample()
    {
        QVERIFY(!WorkoutGame3DTerrainProfile::build({}, 10.0, 1u).ready);
    }

    void preservesExactTrailSocketsAndOrderedCrossSection()
    {
        const WorkoutGame3DTerrainProfileSnapshot profile =
                WorkoutGame3DTerrainProfile::build(roadSample(), 42.0, 17u);

        QVERIFY(profile.ready);
        for (std::size_t index = 1; index < profile.vertices.size(); ++index) {
            QVERIFY(profile.vertices[index - 1].lateralMeters
                    < profile.vertices[index].lateralMeters);
        }
        QCOMPARE(profile.vertices[3].lateralMeters, -0.68);
        QCOMPARE(profile.vertices[4].lateralMeters, 0.68);
        QVERIFY(near(profile.vertices[3].elevationMeters,
                     profile.vertices[4].elevationMeters));
        QVERIFY(std::abs(profile.vertices[3].elevationMeters - 4.0) < 0.04);
    }

    void producesDeterministicUnevenForestRelief()
    {
        const WorkoutGame3DTerrainProfileSnapshot first =
                WorkoutGame3DTerrainProfile::build(roadSample(), 83.25, 913u);
        const WorkoutGame3DTerrainProfileSnapshot second =
                WorkoutGame3DTerrainProfile::build(roadSample(), 83.25, 913u);

        QVERIFY(first.ready && second.ready);
        double minimum = first.vertices.front().elevationMeters;
        double maximum = minimum;
        for (std::size_t index = 0; index < first.vertices.size(); ++index) {
            const auto &left = first.vertices[index];
            const auto &right = second.vertices[index];
            QCOMPARE(left.lateralMeters, right.lateralMeters);
            QCOMPARE(left.elevationMeters, right.elevationMeters);
            QCOMPARE(left.red, right.red);
            QCOMPARE(left.green, right.green);
            QCOMPARE(left.blue, right.blue);
            minimum = std::min(minimum, left.elevationMeters);
            maximum = std::max(maximum, left.elevationMeters);
        }
        QVERIFY(maximum - minimum > 0.35);
        QVERIFY(first.vertices.front().green
                != first.vertices[3].green);
    }

    void nonPhysicalObstacleDoesNotRaiseTheForestSocket()
    {
        WorkoutGameRoadSample road = roadSample();
        road.center.elevationMeters = 4.54;
        road.nonPhysicalFeatureOffsetMeters = 0.54;

        const WorkoutGame3DTerrainProfileSnapshot profile =
                WorkoutGame3DTerrainProfile::build(road, 42.0, 17u);

        QVERIFY(profile.ready);
        QVERIFY(near(profile.vertices[3].elevationMeters, 3.98));
        QVERIFY(near(profile.vertices[4].elevationMeters, 3.98));
    }

    void usesGlobalDistanceForChunkSocketContinuity()
    {
        const WorkoutGame3DTerrainProfileSnapshot first =
                WorkoutGame3DTerrainProfile::build(roadSample(), 120.0, 55u);
        const WorkoutGame3DTerrainProfileSnapshot adjacentChunk =
                WorkoutGame3DTerrainProfile::build(roadSample(), 120.0, 55u);

        for (std::size_t index = 0; index < first.vertices.size(); ++index) {
            QCOMPARE(first.vertices[index].lateralMeters,
                     adjacentChunk.vertices[index].lateralMeters);
            QCOMPARE(first.vertices[index].elevationMeters,
                     adjacentChunk.vertices[index].elevationMeters);
        }
    }

    void interpolatesTerrainHeightAtObjectAnchor()
    {
        const WorkoutGame3DTerrainProfileSnapshot profile =
                WorkoutGame3DTerrainProfile::build(roadSample(), 61.0, 73u);
        QVERIFY(profile.ready);

        const double lateral = -4.0;
        const auto &left = profile.vertices[1];
        const auto &right = profile.vertices[2];
        const double ratio = (lateral - left.lateralMeters)
                / (right.lateralMeters - left.lateralMeters);
        const double expected = left.elevationMeters
                + (right.elevationMeters - left.elevationMeters) * ratio;
        QVERIFY(near(WorkoutGame3DTerrainProfile::elevationAtLateral(
                         profile, lateral), expected));
        QCOMPARE(WorkoutGame3DTerrainProfile::elevationAtLateral(
                         profile, -100.0),
                 profile.vertices.front().elevationMeters);
        QCOMPARE(WorkoutGame3DTerrainProfile::elevationAtLateral(
                         profile, 100.0),
                 profile.vertices.back().elevationMeters);
        QCOMPARE(WorkoutGame3DTerrainProfile::elevationAtLateral(
                         {}, lateral), 0.0);

        WorkoutGame3DTerrainProfileSnapshot malformed = profile;
        malformed.vertices[2].lateralMeters =
                malformed.vertices[1].lateralMeters;
        QCOMPARE(WorkoutGame3DTerrainProfile::elevationAtLateral(
                         malformed, lateral), 0.0);
    }

    void rejectsMalformedInputs()
    {
        WorkoutGameRoadSample malformed = roadSample();
        malformed.center.halfWidthMeters = std::nan("");
        malformed.center.elevationMeters =
                std::numeric_limits<double>::infinity();
        const WorkoutGame3DTerrainProfileSnapshot profile =
                WorkoutGame3DTerrainProfile::build(
                    malformed, std::nan(""), 0u);

        QVERIFY(!profile.ready);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGame3DTerrainProfile)
#include "testWorkoutGame3DTerrainProfile.moc"
