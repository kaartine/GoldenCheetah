/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutRideCommandFilter.h"

#include <QTest>

#include <limits>

class TestWorkoutRideCommandFilter : public QObject
{
    Q_OBJECT

private slots:
    void firstTargetIsDispatchedAndRounded()
    {
        WorkoutRideCommandFilter filter;
        const WorkoutRideCommandDecision result = filter.update(213.6, 200.0, 1000);

        QVERIFY(result.dispatch);
        QVERIFY(result.hasEffectiveTarget);
        QCOMPARE(result.effectiveWatts, 214.0);
        QCOMPARE(result.retryAfterMs, -1);
    }

    void identicalAndSubWattTargetsAreDeduplicated()
    {
        WorkoutRideCommandFilter filter;
        QVERIFY(filter.update(200.0, 200.0, 0).dispatch);

        const WorkoutRideCommandDecision identical = filter.update(200.0, 200.0, 1000);
        const WorkoutRideCommandDecision subWatt = filter.update(200.4, 200.0, 2000);

        QVERIFY(!identical.dispatch);
        QVERIFY(!subWatt.dispatch);
        QCOMPARE(subWatt.effectiveWatts, 200.0);
    }

    void rapidUpdatesAreDeferredAndLatestTargetWins()
    {
        WorkoutRideCommandFilter filter;
        QVERIFY(filter.update(200.0, 200.0, 1000).dispatch);

        const WorkoutRideCommandDecision first = filter.update(220.0, 200.0, 1100);
        const WorkoutRideCommandDecision latest = filter.update(230.0, 200.0, 1200);
        const WorkoutRideCommandDecision dispatched = filter.update(230.0, 200.0, 1250);

        QVERIFY(!first.dispatch);
        QCOMPARE(first.retryAfterMs, 150);
        QVERIFY(!latest.dispatch);
        QCOMPARE(latest.retryAfterMs, 50);
        QVERIFY(dispatched.dispatch);
        QCOMPARE(dispatched.effectiveWatts, 225.0);
        QCOMPARE(dispatched.retryAfterMs, 250);
    }

    void cadenceDrivenChangesAreSlewLimited()
    {
        WorkoutRideCommandFilter filter;
        QVERIFY(filter.update(200.0, 200.0, 0).dispatch);

        const WorkoutRideCommandDecision first = filter.update(400.0, 200.0, 1000);
        const WorkoutRideCommandDecision second = filter.update(400.0, 200.0, 2000);

        QVERIFY(first.dispatch);
        QCOMPARE(first.effectiveWatts, 300.0);
        QCOMPARE(first.retryAfterMs, 250);
        QVERIFY(second.dispatch);
        QCOMPARE(second.effectiveWatts, 400.0);
        QCOMPARE(second.retryAfterMs, -1);
    }

    void authoredWorkoutStepBypassesThrottleAndSlewLimit()
    {
        WorkoutRideCommandFilter filter;
        QVERIFY(filter.update(200.0, 200.0, 1000).dispatch);

        const WorkoutRideCommandDecision result = filter.update(400.0, 400.0, 1050);

        QVERIFY(result.dispatch);
        QCOMPARE(result.effectiveWatts, 400.0);
        QCOMPARE(result.retryAfterMs, -1);
    }

    void resetForcesCurrentTargetToBeResent()
    {
        WorkoutRideCommandFilter filter;
        QVERIFY(filter.update(220.0, 220.0, 1000).dispatch);
        QVERIFY(!filter.update(220.0, 220.0, 2000).dispatch);

        filter.reset();
        const WorkoutRideCommandDecision result = filter.update(220.0, 220.0, 2100);

        QVERIFY(result.dispatch);
        QCOMPARE(result.effectiveWatts, 220.0);
    }

    void backwardsClockStartsAnewDispatchWindow()
    {
        WorkoutRideCommandFilter filter;
        QVERIFY(filter.update(200.0, 200.0, 1000).dispatch);

        const WorkoutRideCommandDecision result = filter.update(240.0, 200.0, 900);

        QVERIFY(result.dispatch);
        QCOMPARE(result.effectiveWatts, 240.0);
    }

    void invalidTargetsAreNeverDispatched()
    {
        WorkoutRideCommandFilter filter;
        const double nan = std::numeric_limits<double>::quiet_NaN();

        QVERIFY(!filter.update(nan, 200.0, 0).dispatch);
        QVERIFY(!filter.update(200.0, nan, 1000).dispatch);
        QVERIFY(!filter.update(-1.0, 200.0, 2000).dispatch);
        QVERIFY(!filter.update(200.0, -1.0, 3000).dispatch);
    }

    void targetIsClampedToTrainerSafetyRange()
    {
        WorkoutRideCommandFilter filter;
        const WorkoutRideCommandDecision result = filter.update(2000.0, 2000.0, 0);

        QVERIFY(result.dispatch);
        QCOMPARE(result.effectiveWatts, 1500.0);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutRideCommandFilter)
#include "testWorkoutRideCommandFilter.moc"
