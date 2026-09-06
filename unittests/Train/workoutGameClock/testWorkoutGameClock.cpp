/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameClock.h"
#include "Train/WorkoutGamePositionRate.h"

#include <QTest>

class TestWorkoutGameClock : public QObject
{
    Q_OBJECT

private slots:
    void fixedDeadlinesDoNotAccumulateWakeupJitter()
    {
        WorkoutGameClock clock(20, 4);
        clock.reset(1000, 100, true, 1.0);

        const WorkoutGameClockAdvance first = clock.advance(121);
        QCOMPARE(first.ticks.size(), std::size_t(1));
        QCOMPARE(first.ticks[0].deadlineMonotonicMs, std::int64_t(120));
        QCOMPARE(first.ticks[0].workoutTimeMs, std::int64_t(1020));

        const WorkoutGameClockAdvance second = clock.advance(143);
        QCOMPARE(second.ticks.size(), std::size_t(1));
        QCOMPARE(second.ticks[0].deadlineMonotonicMs, std::int64_t(140));
        QCOMPARE(second.ticks[0].workoutTimeMs, std::int64_t(1040));
    }

    void catchupIsBoundedAndRealignsToAnAbsoluteDeadline()
    {
        WorkoutGameClock clock(20, 3);
        clock.reset(0, 0, true, 1.0);

        const WorkoutGameClockAdvance delayed = clock.advance(205);
        QCOMPARE(delayed.ticks.size(), std::size_t(3));
        QVERIFY(delayed.skippedTicks > 0);
        QCOMPARE(delayed.ticks.back().deadlineMonotonicMs, std::int64_t(200));

        const WorkoutGameClockAdvance next = clock.advance(220);
        QCOMPARE(next.ticks.size(), std::size_t(1));
        QCOMPARE(next.ticks[0].deadlineMonotonicMs, std::int64_t(220));
    }

    void lateForwardAnchorNeverMovesPublishedTimeBackward()
    {
        WorkoutGameClock clock(20, 4);
        clock.reset(1000, 1000, true, 1.0);
        QCOMPARE(clock.positionAt(1500), std::int64_t(1500));

        clock.setAnchor(1450, 1500, true, 1.0);
        QVERIFY(clock.positionAt(1500) >= 1500);
        QVERIFY(clock.positionAt(1700) >= clock.positionAt(1500));
    }

    void smallOutOfOrderAnchorCannotMovePublishedTimeBackward()
    {
        WorkoutGameClock clock(20, 4);
        clock.reset(1000, 1000, true, 1.0);
        clock.setAnchor(1500, 1500, true, 1.0);
        const std::int64_t before = clock.positionAt(1520);

        clock.setAnchor(1490, 1520, true, 1.0);
        QVERIFY(clock.positionAt(1520) >= before);
        QVERIFY(clock.positionAt(1600) >= clock.positionAt(1520));
    }

    void quantizedLaggingSourceCannotResetOrStallRunningTime()
    {
        WorkoutGameClock clock(20, 4);
        clock.reset(0, 0, true, 1.0);
        const WorkoutGameClockAdvance initial = clock.advance(1200);
        QVERIFY(!initial.ticks.empty());
        const std::int64_t beforeLaggingAnchor =
                initial.ticks.back().workoutTimeMs;

        // Distance playback may still report zero until its first whole
        // distance sample even though the rider is already moving.
        clock.setAnchor(0, 1200, true, 1.0);
        QVERIFY(clock.positionAt(1200) >= beforeLaggingAnchor);
        const WorkoutGameClockAdvance afterLaggingAnchor =
                clock.advance(1400);
        QVERIFY(!afterLaggingAnchor.ticks.empty());
        QVERIFY(afterLaggingAnchor.ticks.back().workoutTimeMs
                > beforeLaggingAnchor);

        const std::int64_t beforeSlowCorrection =
                afterLaggingAnchor.ticks.back().workoutTimeMs;
        clock.setAnchor(900, 2000, true, 0.2);
        const WorkoutGameClockAdvance afterSlowCorrection =
                clock.advance(2200);
        QVERIFY(!afterSlowCorrection.ticks.empty());
        QVERIFY(afterSlowCorrection.ticks.back().workoutTimeMs
                > beforeSlowCorrection);
    }

    void distanceDrivenRateFollowsObservedAnchorsWithoutPositionJump()
    {
        WorkoutGameClock clock(20, 4);
        clock.reset(1000, 1000, true, 0.5);
        const std::int64_t before = clock.positionAt(2000);
        QCOMPARE(before, std::int64_t(1500));

        clock.setAnchor(1600, 2000, true, 0.6);
        QCOMPARE(clock.positionAt(2000), before);
        QVERIFY(clock.positionAt(2200) > before);
    }

    void pauseFreezesPositionAndResumeDoesNotCatchUpWallTime()
    {
        WorkoutGameClock clock(20, 4);
        clock.reset(1000, 1000, true, 1.0);
        clock.setRunning(false, 1500);
        QCOMPARE(clock.positionAt(5000), std::int64_t(1500));

        clock.setRunning(true, 5000);
        QCOMPARE(clock.positionAt(5000), std::int64_t(1500));
        QCOMPARE(clock.positionAt(5100), std::int64_t(1600));
    }

    void pauseAndResumeAnchorsRetireTheOldDeadline()
    {
        WorkoutGameClock clock(20, 4);
        clock.reset(1000, 1000, true, 1.0);
        QCOMPARE(clock.advance(1080).skippedTicks, std::size_t(0));

        clock.setAnchor(1100, 1100, false, 1.0);
        QVERIFY(clock.advance(5000).ticks.empty());

        clock.setAnchor(1100, 5000, true, 1.0);
        const WorkoutGameClockAdvance resumed = clock.advance(5020);
        QCOMPARE(resumed.skippedTicks, std::size_t(0));
        QCOMPARE(resumed.ticks.size(), std::size_t(1));
        QCOMPARE(resumed.ticks.front().deadlineMonotonicMs,
                 std::int64_t(5020));
    }

    void quantizedDistanceDoesNotAlternateBetweenStopAndBurst()
    {
        WorkoutGamePositionRate estimator;
        estimator.reset(1.0);
        QCOMPARE(estimator.update({1000, 1000, true, true}), 1.0);

        const double repeated = estimator.update({1000, 1150, true, true});
        QVERIFY(repeated > 0.5);
        const double advanced = estimator.update({1200, 1200, true, true});
        QVERIFY(advanced > 0.5);
        QVERIFY(advanced < 2.0);
        QCOMPARE(estimator.update({1200, 1250, true, true}), advanced);
    }

    void explicitStationaryTelemetryStopsDistancePresentation()
    {
        WorkoutGamePositionRate estimator;
        estimator.reset(1.0);
        estimator.update({1000, 1000, true, true});
        QCOMPARE(estimator.update({1000, 1100, true, false}), 0.0);
        QVERIFY(estimator.update({1000, 1150, true, true}) > 0.0);
        QVERIFY(estimator.update({1200, 1300, true, true}) > 0.0);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameClock)
#include "testWorkoutGameClock.moc"
