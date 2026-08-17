/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameWorkoutAdapter.h"

#include <QTest>

#include <limits>

class TestWorkoutGameWorkoutAdapter : public QObject
{
    Q_OBJECT

private slots:
    void emptyPointsProduceExplicitFallback()
    {
        const WorkoutGameWorkout workout = WorkoutGameWorkoutAdapter::normalize({});

        QCOMPARE(workout.status, WorkoutGameWorkoutStatus::EmptyWorkout);
        QVERIFY(workout.intervals.empty());
    }

    void constantPowerPointsBecomeOneInterval()
    {
        const WorkoutGameWorkout workout = WorkoutGameWorkoutAdapter::normalize({
            {0.0, 180.0}, {60000.0, 180.0}
        });

        QCOMPARE(workout.status, WorkoutGameWorkoutStatus::Ready);
        QCOMPARE(workout.intervals.size(), std::size_t(1));
        QCOMPARE(workout.intervals[0].startMs, std::int64_t(0));
        QCOMPARE(workout.intervals[0].durationMs, std::int64_t(60000));
        QCOMPARE(workout.intervals[0].startWatts, 180.0);
        QCOMPARE(workout.intervals[0].endWatts, 180.0);
    }

    void rampEndpointsArePreserved()
    {
        const WorkoutGameWorkout workout = WorkoutGameWorkoutAdapter::normalize({
            {0.0, 100.0}, {60000.0, 200.0}
        });

        QCOMPARE(workout.intervals[0].startWatts, 100.0);
        QCOMPARE(workout.intervals[0].endWatts, 200.0);
    }

    void duplicateTimestampRepresentsInstantPowerStep()
    {
        const WorkoutGameWorkout workout = WorkoutGameWorkoutAdapter::normalize({
            {0.0, 100.0},
            {60000.0, 100.0},
            {60000.0, 250.0},
            {70000.0, 250.0}
        });

        QCOMPARE(workout.intervals.size(), std::size_t(2));
        QCOMPARE(workout.intervals[0].startWatts, 100.0);
        QCOMPARE(workout.intervals[0].endWatts, 100.0);
        QCOMPARE(workout.intervals[1].startMs, std::int64_t(60000));
        QCOMPARE(workout.intervals[1].startWatts, 250.0);
        QCOMPARE(workout.intervals[1].endWatts, 250.0);
    }

    void delayedFirstPointCreatesLeadingConstantInterval()
    {
        const WorkoutGameWorkout workout = WorkoutGameWorkoutAdapter::normalize({
            {10000.0, 120.0}, {20000.0, 150.0}
        });

        QCOMPARE(workout.intervals.size(), std::size_t(2));
        QCOMPARE(workout.intervals[0].startMs, std::int64_t(0));
        QCOMPARE(workout.intervals[0].durationMs, std::int64_t(10000));
        QCOMPARE(workout.intervals[0].startWatts, 120.0);
        QCOMPARE(workout.intervals[0].endWatts, 120.0);
        QCOMPARE(workout.intervals[1].startMs, std::int64_t(10000));
    }

    void subMillisecondTimesAreRoundedConsistently()
    {
        const WorkoutGameWorkout workout = WorkoutGameWorkoutAdapter::normalize({
            {0.4, 100.0}, {999.6, 100.0}
        });

        QCOMPARE(workout.status, WorkoutGameWorkoutStatus::Ready);
        QCOMPARE(workout.intervals[0].startMs, std::int64_t(0));
        QCOMPARE(workout.intervals[0].durationMs, std::int64_t(1000));
    }

    void invalidPointIsRejected_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("negative-time") << 0;
        QTest::newRow("decreasing-time") << 1;
        QTest::newRow("negative-power") << 2;
        QTest::newRow("nan-time") << 3;
        QTest::newRow("infinite-power") << 4;
        QTest::newRow("time-overflow") << 5;
    }

    void invalidPointIsRejected()
    {
        QFETCH(int, kind);
        std::vector<WorkoutGamePowerPoint> points = {
            {0.0, 100.0}, {1000.0, 100.0}
        };
        switch (kind) {
        case 0: points[0].timeMs = -1.0; break;
        case 1: points[1].timeMs = -0.1; break;
        case 2: points[0].watts = -1.0; break;
        case 3: points[0].timeMs = std::numeric_limits<double>::quiet_NaN(); break;
        case 4: points[1].watts = std::numeric_limits<double>::infinity(); break;
        default: points[1].timeMs = 9223372036854775808.0; break;
        }

        const WorkoutGameWorkout workout = WorkoutGameWorkoutAdapter::normalize(points);

        QCOMPARE(workout.status, WorkoutGameWorkoutStatus::InvalidPoint);
        QVERIFY(workout.intervals.empty());
    }

    void zeroDurationOnlyPointsRemainEmpty()
    {
        const WorkoutGameWorkout workout = WorkoutGameWorkoutAdapter::normalize({
            {0.0, 100.0}, {0.0, 200.0}
        });

        QCOMPARE(workout.status, WorkoutGameWorkoutStatus::EmptyWorkout);
        QVERIFY(workout.intervals.empty());
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameWorkoutAdapter)
#include "testWorkoutGameWorkoutAdapter.moc"
