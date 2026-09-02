/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameGapJumpSelector.h"

#include <QTest>

#include <cmath>
#include <limits>

class TestWorkoutGameGapJumpSelector : public QObject
{
    Q_OBJECT

private slots:
    void coldSelectionIsExactAtBoundarySpeeds()
    {
        const double belowShort = std::nextafter(4.0, 0.0);
        const double belowMedium = std::nextafter(5.2, 0.0);
        const double belowLong = std::nextafter(6.6, 0.0);

        QCOMPARE(WorkoutGameGapJumpSelector::coldSelection(belowShort),
                 WorkoutGameGapJumpLine::None);
        QCOMPARE(WorkoutGameGapJumpSelector::coldSelection(4.0),
                 WorkoutGameGapJumpLine::Short);
        QCOMPARE(WorkoutGameGapJumpSelector::coldSelection(belowMedium),
                 WorkoutGameGapJumpLine::Short);
        QCOMPARE(WorkoutGameGapJumpSelector::coldSelection(5.2),
                 WorkoutGameGapJumpLine::Medium);
        QCOMPARE(WorkoutGameGapJumpSelector::coldSelection(belowLong),
                 WorkoutGameGapJumpLine::Medium);
        QCOMPARE(WorkoutGameGapJumpSelector::coldSelection(6.6),
                 WorkoutGameGapJumpLine::Long);
    }

    void selectionUsesTimedSchmittThresholds()
    {
        WorkoutGameGapJumpSelector selector;
        QCOMPARE(selector.update(4.0, 50).provisionalLine,
                 WorkoutGameGapJumpLine::Short);

        for (int elapsed = 0; elapsed < 250; elapsed += 50) {
            QCOMPARE(selector.update(5.55, 50).provisionalLine,
                     WorkoutGameGapJumpLine::Short);
        }
        QCOMPARE(selector.update(5.55, 50).provisionalLine,
                 WorkoutGameGapJumpLine::Medium);

        for (int elapsed = 0; elapsed < 450; elapsed += 50) {
            QCOMPARE(selector.update(
                         std::nextafter(4.85, 0.0), 50).provisionalLine,
                     WorkoutGameGapJumpLine::Medium);
        }
        QCOMPARE(selector.update(std::nextafter(4.85, 0.0), 50)
                         .provisionalLine,
                 WorkoutGameGapJumpLine::Short);
    }

    void directPromotionToLongDoesNotExposeAnIntermediateLine()
    {
        WorkoutGameGapJumpSelector selector;
        QCOMPARE(selector.update(4.0, 50).provisionalLine,
                 WorkoutGameGapJumpLine::Short);

        for (int elapsed = 0; elapsed < 250; elapsed += 50) {
            QCOMPARE(selector.update(6.95, 50).provisionalLine,
                     WorkoutGameGapJumpLine::Short);
        }
        QCOMPARE(selector.update(6.95, 50).provisionalLine,
                 WorkoutGameGapJumpLine::Long);
    }

    void everySchmittBoundaryUsesItsExactInclusiveRule()
    {
        WorkoutGameGapJumpSelector fallback;
        fallback.update(3.9, 50);
        for (int elapsed = 0; elapsed < 300; elapsed += 50) {
            const auto state = fallback.update(4.35, 50);
            QCOMPARE(state.provisionalLine,
                     elapsed == 250 ? WorkoutGameGapJumpLine::Short
                                    : WorkoutGameGapJumpLine::None);
        }

        WorkoutGameGapJumpSelector shortLine;
        shortLine.update(4.0, 50);
        for (int elapsed = 0; elapsed < 500; elapsed += 50) {
            const auto state = shortLine.update(
                    std::nextafter(3.65, 0.0), 50);
            QCOMPARE(state.provisionalLine,
                     elapsed == 450 ? WorkoutGameGapJumpLine::None
                                    : WorkoutGameGapJumpLine::Short);
        }

        WorkoutGameGapJumpSelector mediumLine;
        mediumLine.update(5.2, 50);
        for (int elapsed = 0; elapsed < 300; elapsed += 50) {
            const auto state = mediumLine.update(6.95, 50);
            QCOMPARE(state.provisionalLine,
                     elapsed == 250 ? WorkoutGameGapJumpLine::Long
                                    : WorkoutGameGapJumpLine::Medium);
        }

        WorkoutGameGapJumpSelector longLine;
        longLine.update(6.6, 50);
        for (int elapsed = 0; elapsed < 500; elapsed += 50) {
            const auto state = longLine.update(
                    std::nextafter(6.25, 0.0), 50);
            QCOMPARE(state.provisionalLine,
                     elapsed == 450 ? WorkoutGameGapJumpLine::Medium
                                    : WorkoutGameGapJumpLine::Long);
        }

        WorkoutGameGapJumpSelector exactRelease;
        exactRelease.update(6.6, 50);
        for (int elapsed = 0; elapsed < 1000; elapsed += 50) {
            QCOMPARE(exactRelease.update(6.25, 50).provisionalLine,
                     WorkoutGameGapJumpLine::Long);
        }
    }

    void equivalentFixedStepDurationGroupingsReachTheSameDecision()
    {
        const int groupings[][3] = {
            {50, 50, 6},
            {100, 50, 4},
            {150, 150, 2},
            {250, 50, 2}
        };
        for (const auto &grouping : groupings) {
            WorkoutGameGapJumpSelector selector;
            selector.update(4.0, 50);
            int elapsed = 0;
            int sample = 0;
            while (elapsed < 300) {
                const int duration = sample == grouping[2] - 1
                        ? 300 - elapsed
                        : grouping[sample % 2];
                selector.update(5.55, duration);
                elapsed += duration;
                ++sample;
            }
            QCOMPARE(elapsed, 300);
            QCOMPARE(selector.state().provisionalLine,
                     WorkoutGameGapJumpLine::Medium);
        }
    }

    void noiseInsideTheSchmittBandCannotOscillateSelection()
    {
        WorkoutGameGapJumpSelector selector;
        QCOMPARE(selector.update(5.2, 50).provisionalLine,
                 WorkoutGameGapJumpLine::Medium);

        for (int sample = 0; sample < 200; ++sample) {
            const double speed = sample % 2 == 0 ? 5.54 : 4.85;
            QCOMPARE(selector.update(speed, 50).provisionalLine,
                     WorkoutGameGapJumpLine::Medium);
        }
    }

    void decisionLocksOnceForItsActionId()
    {
        WorkoutGameGapJumpSelector selector;
        QCOMPARE(selector.update(6.6, 50).provisionalLine,
                 WorkoutGameGapJumpLine::Long);

        const auto locked = selector.lock(42u, true, true);
        QVERIFY(locked.locked);
        QCOMPARE(locked.actionId, std::uint64_t(42));
        QCOMPARE(locked.lockedLine, WorkoutGameGapJumpLine::Long);

        selector.update(0.0, 1000);
        const auto unchanged = selector.lock(99u, false, false);
        QCOMPARE(unchanged.actionId, std::uint64_t(42));
        QCOMPARE(unchanged.lockedLine, WorkoutGameGapJumpLine::Long);
        QCOMPARE(unchanged.provisionalLine, WorkoutGameGapJumpLine::Long);
    }

    void failedReadinessOrStaleTelemetryLocksFallback()
    {
        WorkoutGameGapJumpSelector effortFailure;
        effortFailure.update(6.6, 50);
        QCOMPARE(effortFailure.lock(7u, false, true).lockedLine,
                 WorkoutGameGapJumpLine::None);

        WorkoutGameGapJumpSelector stale;
        stale.update(6.6, 50);
        QCOMPARE(stale.lock(8u, true, false).lockedLine,
                 WorkoutGameGapJumpLine::None);
    }

    void invalidInputsFailClosedAndDurationsAreBounded()
    {
        for (double speed : {
                 -1.0,
                 std::numeric_limits<double>::quiet_NaN(),
                 std::numeric_limits<double>::infinity()}) {
            WorkoutGameGapJumpSelector selector;
            QCOMPARE(selector.update(speed, 50).provisionalLine,
                     WorkoutGameGapJumpLine::None);
        }

        WorkoutGameGapJumpSelector selector;
        QCOMPARE(selector.update(4.0, 50).provisionalLine,
                 WorkoutGameGapJumpLine::Short);
        QCOMPARE(selector.update(5.55, -1).provisionalLine,
                 WorkoutGameGapJumpLine::Short);
        QCOMPARE(selector.update(5.55, 1000000).provisionalLine,
                 WorkoutGameGapJumpLine::Short);
    }

    void resetCannotLeakACommittedLine()
    {
        WorkoutGameGapJumpSelector selector;
        selector.update(6.6, 50);
        selector.lock(11u, true, true);

        selector.reset();
        const auto state = selector.state();
        QVERIFY(!state.locked);
        QCOMPARE(state.actionId, std::uint64_t(0));
        QCOMPARE(state.provisionalLine, WorkoutGameGapJumpLine::None);
        QCOMPARE(state.lockedLine, WorkoutGameGapJumpLine::None);
        QCOMPARE(selector.update(4.0, 50).provisionalLine,
                 WorkoutGameGapJumpLine::Short);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameGapJumpSelector)
#include "testWorkoutGameGapJumpSelector.moc"
