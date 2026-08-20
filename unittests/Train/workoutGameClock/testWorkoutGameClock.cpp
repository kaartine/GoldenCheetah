/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameClock.h"

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
};

QTEST_GUILESS_MAIN(TestWorkoutGameClock)
#include "testWorkoutGameClock.moc"
