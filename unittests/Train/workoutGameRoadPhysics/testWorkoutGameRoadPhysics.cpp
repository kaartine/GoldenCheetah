/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameRoadPhysics.h"

#include <QTest>

#include <cmath>
#include <limits>

namespace {

WorkoutGameRoadPhysicsSnapshot ride(
        WorkoutGameRoadPhysics &physics,
        double powerWatts,
        double gradePercent,
        int seconds)
{
    WorkoutGameRoadPhysicsSnapshot result;
    for (int second = 0; second < seconds; ++second) {
        result = physics.update({powerWatts, gradePercent, 0.0}, 1000);
    }
    return result;
}

}

class TestWorkoutGameRoadPhysics : public QObject
{
    Q_OBJECT

private slots:
    void defaultParametersAreValid()
    {
        WorkoutGameRoadPhysics physics;

        QVERIFY(physics.configure(WorkoutGameRoadPhysicsParameters()));
        QVERIFY(physics.update({}, 0).ready);
    }

    void invalidParametersAreRejected_data()
    {
        QTest::addColumn<int>("parameter");
        QTest::newRow("mass") << 0;
        QTest::newRow("drag-area") << 1;
        QTest::newRow("rolling-resistance") << 2;
        QTest::newRow("air-density") << 3;
        QTest::newRow("efficiency") << 4;
        QTest::newRow("rotating-mass") << 5;
        QTest::newRow("low-speed") << 6;
        QTest::newRow("drive-force") << 7;
        QTest::newRow("brake-force") << 8;
        QTest::newRow("maximum-speed") << 9;
    }

    void invalidParametersAreRejected()
    {
        QFETCH(int, parameter);
        WorkoutGameRoadPhysicsParameters parameters;
        switch (parameter) {
        case 0: parameters.totalMassKg = 0.0; break;
        case 1: parameters.dragAreaSquareMeters = -1.0; break;
        case 2: parameters.rollingResistanceCoefficient = -1.0; break;
        case 3: parameters.airDensityKgPerCubicMeter = 0.0; break;
        case 4: parameters.drivetrainEfficiency = 1.1; break;
        case 5: parameters.rotatingMassFactor = 0.0; break;
        case 6: parameters.lowSpeedMetersPerSecond = 0.0; break;
        case 7: parameters.maximumDriveForceNewtons = 0.0; break;
        case 8: parameters.maximumBrakeForceNewtons = 0.0; break;
        default: parameters.maximumSpeedMetersPerSecond = 0.0; break;
        }

        WorkoutGameRoadPhysics physics;
        QVERIFY(!physics.configure(parameters));
        QVERIFY(!physics.update({}, 1000).ready);
    }

    void flatRoadDoesNotMoveWithoutPower()
    {
        WorkoutGameRoadPhysics physics;
        QVERIFY(physics.configure(WorkoutGameRoadPhysicsParameters()));

        const WorkoutGameRoadPhysicsSnapshot result = ride(
                physics, 0.0, 0.0, 60);

        QCOMPARE(result.speedMetersPerSecond, 0.0);
        QCOMPARE(result.distanceMeters, 0.0);
        QCOMPARE(result.elevationMeters, 0.0);
    }

    void measuredPowerMovesTheBikeOnFlatRoad()
    {
        WorkoutGameRoadPhysics physics;
        QVERIFY(physics.configure(WorkoutGameRoadPhysicsParameters()));

        const WorkoutGameRoadPhysicsSnapshot result = ride(
                physics, 200.0, 0.0, 60);

        QVERIFY(result.speedMetersPerSecond > 6.0);
        QVERIFY(result.speedMetersPerSecond < 9.0);
        QVERIFY(result.distanceMeters > 300.0);
        QCOMPARE(result.elevationMeters, 0.0);
    }

    void coastingOnFlatRoadEventuallyStops()
    {
        WorkoutGameRoadPhysics physics;
        QVERIFY(physics.configure(WorkoutGameRoadPhysicsParameters()));
        ride(physics, 200.0, 0.0, 60);

        const WorkoutGameRoadPhysicsSnapshot result = ride(
                physics, 0.0, 0.0, 120);

        QCOMPARE(result.speedMetersPerSecond, 0.0);
    }

    void gradientChangesSpeedAtTheSamePower()
    {
        WorkoutGameRoadPhysics downhill;
        WorkoutGameRoadPhysics flat;
        WorkoutGameRoadPhysics uphill;
        QVERIFY(downhill.configure(WorkoutGameRoadPhysicsParameters()));
        QVERIFY(flat.configure(WorkoutGameRoadPhysicsParameters()));
        QVERIFY(uphill.configure(WorkoutGameRoadPhysicsParameters()));

        const double downhillSpeed = ride(downhill, 180.0, -5.0, 120)
                .speedMetersPerSecond;
        const double flatSpeed = ride(flat, 180.0, 0.0, 120)
                .speedMetersPerSecond;
        const double uphillSpeed = ride(uphill, 180.0, 5.0, 120)
                .speedMetersPerSecond;

        QVERIFY(downhillSpeed > flatSpeed);
        QVERIFY(flatSpeed > uphillSpeed);
        QVERIFY(uphillSpeed > 0.0);
    }

    void gravityAcceleratesTheBikeDownhillWithoutPower()
    {
        WorkoutGameRoadPhysics physics;
        QVERIFY(physics.configure(WorkoutGameRoadPhysicsParameters()));

        const WorkoutGameRoadPhysicsSnapshot result = ride(
                physics, 0.0, -5.0, 60);

        QVERIFY(result.speedMetersPerSecond > 8.0);
        QVERIFY(result.distanceMeters > 300.0);
        QVERIFY(result.elevationMeters < -10.0);
    }

    void steepUphillNeverRollsBackwards()
    {
        WorkoutGameRoadPhysics physics;
        QVERIFY(physics.configure(WorkoutGameRoadPhysicsParameters()));

        const WorkoutGameRoadPhysicsSnapshot result = ride(
                physics, 0.0, 20.0, 60);

        QCOMPARE(result.speedMetersPerSecond, 0.0);
        QCOMPARE(result.distanceMeters, 0.0);
    }

    void fixedStepIsIndependentOfUpdateChunkSize()
    {
        WorkoutGameRoadPhysics frequent;
        WorkoutGameRoadPhysics sparse;
        QVERIFY(frequent.configure(WorkoutGameRoadPhysicsParameters()));
        QVERIFY(sparse.configure(WorkoutGameRoadPhysicsParameters()));

        WorkoutGameRoadPhysicsSnapshot frequentResult;
        WorkoutGameRoadPhysicsSnapshot sparseResult;
        for (int update = 0; update < 1000; ++update) {
            frequentResult = frequent.update({210.0, 3.0, 0.0}, 10);
        }
        for (int update = 0; update < 100; ++update) {
            sparseResult = sparse.update({210.0, 3.0, 0.0}, 100);
        }

        QCOMPARE(frequentResult.speedMetersPerSecond,
                 sparseResult.speedMetersPerSecond);
        QCOMPARE(frequentResult.distanceMeters, sparseResult.distanceMeters);
        QCOMPARE(frequentResult.elevationMeters, sparseResult.elevationMeters);
    }

    void partialStepUsesTheCurrentTelemetrySample()
    {
        WorkoutGameRoadPhysics physics;
        QVERIFY(physics.configure(WorkoutGameRoadPhysicsParameters()));

        physics.update({0.0, 0.0, 0.0}, 5);
        const WorkoutGameRoadPhysicsSnapshot result = physics.update(
                {300.0, 0.0, 0.0}, 5);

        QCOMPARE(result.elapsedTimeMs, std::int64_t(10));
        QVERIFY(result.speedMetersPerSecond > 0.0);
        QVERIFY(result.speedMetersPerSecond < 0.015);
    }

    void invalidInputIsSanitizedAndBounded()
    {
        WorkoutGameRoadPhysics physics;
        QVERIFY(physics.configure(WorkoutGameRoadPhysicsParameters()));
        const double nan = std::numeric_limits<double>::quiet_NaN();

        const WorkoutGameRoadPhysicsSnapshot result = physics.update(
                {nan, nan, nan}, 60000);

        QVERIFY(std::isfinite(result.speedMetersPerSecond));
        QVERIFY(std::isfinite(result.distanceMeters));
        QVERIFY(std::isfinite(result.elevationMeters));
        QVERIFY(result.speedMetersPerSecond >= 0.0);
        QCOMPARE(result.elapsedTimeMs, std::int64_t(10000));
        QCOMPARE(result.droppedUpdateTimeMs, std::int64_t(50000));
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameRoadPhysics)
#include "testWorkoutGameRoadPhysics.moc"
