/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameGapJumpLaunchWindow.h"

#include <QTest>

#include <cmath>
#include <limits>

namespace {

void compareNear(double actual, double expected, double tolerance = 1e-12)
{
    QVERIFY2(std::abs(actual - expected) <= tolerance,
             qPrintable(QStringLiteral("actual=%1 expected=%2")
                        .arg(actual, 0, 'g', 16)
                        .arg(expected, 0, 'g', 16)));
}

}

class TestWorkoutGameGapJumpLaunchWindow : public QObject
{
    Q_OBJECT

private slots:
    void oneSampleSpikeIsTimeWeighted()
    {
        WorkoutGameGapJumpLaunchWindow window;
        window.addSample(5.0, 250.0, 200.0, 499);
        window.addSample(100.0, 250.0, 200.0, 1);

        const auto metrics = window.metrics();
        QVERIFY(metrics.hasFullSpeedWindow());
        compareNear(metrics.rollingSpeedAverageMetersPerSecond(), 5.19);
        compareNear(metrics.bestFullWindowSpeedAverageMetersPerSecond(),
                    5.19);
        QVERIFY(metrics.rollingSpeedAverageMetersPerSecond() < 6.6);
    }

    void exactFiveHundredMillisecondBoundaryCompletesWindow()
    {
        WorkoutGameGapJumpLaunchWindow window;
        window.addSample(4.0, 200.0, 200.0, 499);

        auto metrics = window.metrics();
        QVERIFY(!metrics.hasFullSpeedWindow());
        QCOMPARE(metrics.rollingSpeedDurationMilliseconds(), 499);
        QCOMPARE(metrics.atOrAboveTargetPowerHoldMilliseconds(), 499);

        window.addSample(9.0, 200.0, 200.0, 1);
        metrics = window.metrics();
        QVERIFY(metrics.hasFullSpeedWindow());
        QCOMPARE(metrics.rollingSpeedDurationMilliseconds(), 500);
        QCOMPARE(metrics.atOrAboveTargetPowerHoldMilliseconds(), 500);
        compareNear(metrics.rollingSpeedAverageMetersPerSecond(), 4.01);
    }

    void weightedAndBatchedSamplesAreEquivalent()
    {
        WorkoutGameGapJumpLaunchWindow batched;
        batched.addSample(4.0, 220.0, 200.0, 200);
        batched.addSample(8.0, 220.0, 200.0, 300);

        WorkoutGameGapJumpLaunchWindow stepped;
        for (int elapsed = 0; elapsed < 200; elapsed += 20) {
            stepped.addSample(4.0, 220.0, 200.0, 20);
        }
        for (int elapsed = 0; elapsed < 300; elapsed += 20) {
            stepped.addSample(8.0, 220.0, 200.0, 20);
        }

        const auto batchedMetrics = batched.metrics();
        const auto steppedMetrics = stepped.metrics();
        compareNear(batchedMetrics.rollingSpeedAverageMetersPerSecond(), 6.4);
        compareNear(steppedMetrics.rollingSpeedAverageMetersPerSecond(),
                    batchedMetrics.rollingSpeedAverageMetersPerSecond());
        compareNear(steppedMetrics.bestFullWindowSpeedAverageMetersPerSecond(),
                    batchedMetrics.bestFullWindowSpeedAverageMetersPerSecond());
        QCOMPARE(steppedMetrics.atOrAboveTargetPowerHoldMilliseconds(),
                 batchedMetrics.atOrAboveTargetPowerHoldMilliseconds());
    }

    void rollingWindowEvictsOldestDurationAndRetainsBestWindow()
    {
        WorkoutGameGapJumpLaunchWindow window;
        window.addSample(4.0, 200.0, 200.0, 500);
        window.addSample(8.0, 200.0, 200.0, 250);

        auto metrics = window.metrics();
        compareNear(metrics.rollingSpeedAverageMetersPerSecond(), 6.0);
        compareNear(metrics.bestFullWindowSpeedAverageMetersPerSecond(), 6.0);

        window.addSample(2.0, 200.0, 200.0, 250);
        metrics = window.metrics();
        compareNear(metrics.rollingSpeedAverageMetersPerSecond(), 5.0);
        compareNear(metrics.bestFullWindowSpeedAverageMetersPerSecond(), 6.0);
        QCOMPARE(metrics.rollingSpeedDurationMilliseconds(), 500);
    }

    void powerHoldMustBeContinuousAndIsCapped()
    {
        WorkoutGameGapJumpLaunchWindow window;
        window.addSample(5.0, 200.0, 200.0, 300);
        QCOMPARE(window.metrics().atOrAboveTargetPowerHoldMilliseconds(), 300);

        window.addSample(5.0, 250.0, 200.0, 400);
        QCOMPARE(window.metrics().atOrAboveTargetPowerHoldMilliseconds(), 500);

        window.addSample(5.0, 199.0, 200.0, 1);
        QCOMPARE(window.metrics().atOrAboveTargetPowerHoldMilliseconds(), 0);

        window.addSample(5.0, 201.0, 200.0, 125);
        QCOMPARE(window.metrics().atOrAboveTargetPowerHoldMilliseconds(), 125);
    }

    void invalidTelemetryFailsClosed()
    {
        const double invalidValues[] = {
            -1.0,
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity()
        };
        for (double invalidSpeed : invalidValues) {
            WorkoutGameGapJumpLaunchWindow window;
            window.addSample(6.6, 250.0, 200.0, 500);
            window.addSample(invalidSpeed, 250.0, 200.0, 10);
            const auto metrics = window.metrics();
            QVERIFY(!metrics.telemetryValid());
            QVERIFY(!metrics.hasFullSpeedWindow());
            QCOMPARE(metrics.rollingSpeedDurationMilliseconds(), 0);
            QCOMPARE(metrics.atOrAboveTargetPowerHoldMilliseconds(), 0);
        }

        WorkoutGameGapJumpLaunchWindow invalidActual;
        invalidActual.addSample(6.6, 250.0, 200.0, 500);
        invalidActual.addSample(6.6, -1.0, 200.0, 10);
        QVERIFY(!invalidActual.metrics().telemetryValid());
        QVERIFY(!invalidActual.metrics().hasFullSpeedWindow());

        WorkoutGameGapJumpLaunchWindow invalidTarget;
        invalidTarget.addSample(6.6, 250.0, 200.0, 500);
        invalidTarget.addSample(6.6, 250.0, 0.0, 10);
        QVERIFY(!invalidTarget.metrics().telemetryValid());
        QCOMPARE(invalidTarget.metrics()
                         .atOrAboveTargetPowerHoldMilliseconds(), 0);

        WorkoutGameGapJumpLaunchWindow ignoredDuration;
        ignoredDuration.addSample(5.0, 220.0, 200.0, 100);
        ignoredDuration.addSample(-1.0, -1.0, 0.0, 0);
        ignoredDuration.addSample(-1.0, -1.0, 0.0, 2001);
        const auto ignoredMetrics = ignoredDuration.metrics();
        QVERIFY(ignoredMetrics.telemetryValid());
        QCOMPARE(ignoredMetrics.rollingSpeedDurationMilliseconds(), 100);
        QCOMPARE(ignoredMetrics.atOrAboveTargetPowerHoldMilliseconds(), 100);
    }

    void resetClearsAllMetrics()
    {
        WorkoutGameGapJumpLaunchWindow window;
        window.addSample(6.6, 250.0, 200.0, 500);
        window.reset();

        const auto metrics = window.metrics();
        QVERIFY(!metrics.telemetryValid());
        QVERIFY(!metrics.hasFullSpeedWindow());
        QCOMPARE(metrics.rollingSpeedDurationMilliseconds(), 0);
        QCOMPARE(metrics.atOrAboveTargetPowerHoldMilliseconds(), 0);
        QCOMPARE(metrics.rollingSpeedAverageMetersPerSecond(), 0.0);
        QCOMPARE(metrics.bestFullWindowSpeedAverageMetersPerSecond(), 0.0);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameGapJumpLaunchWindow)
#include "testWorkoutGameGapJumpLaunchWindow.moc"
