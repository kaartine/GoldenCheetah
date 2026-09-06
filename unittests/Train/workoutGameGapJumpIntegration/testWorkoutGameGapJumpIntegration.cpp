/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameCourseDocument.h"
#include "Train/WorkoutGameGapJumpGeometry.h"
#include "Train/WorkoutGameGapJumpSelector.h"
#include "Train/WorkoutGameWorld.h"

#include <QTest>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

constexpr int FixedStepMilliseconds = 50;

bool near(double left, double right, double tolerance = 1e-12)
{
    return std::abs(left - right) <= tolerance;
}

WorkoutGameGapJumpSelectionState holdSpeed(
        WorkoutGameGapJumpSelector &selector,
        double speedMetersPerSecond,
        int durationMilliseconds)
{
    Q_ASSERT(durationMilliseconds % FixedStepMilliseconds == 0);
    WorkoutGameGapJumpSelectionState state = selector.state();
    for (int elapsed = 0; elapsed < durationMilliseconds;
         elapsed += FixedStepMilliseconds) {
        state = selector.update(speedMetersPerSecond,
                                FixedStepMilliseconds);
    }
    return state;
}

}

class TestWorkoutGameGapJumpIntegration : public QObject
{
    Q_OBJECT

private slots:
    void canonicalProfileDefinesThreeDistinctSpeedMatchedGaps()
    {
        const auto profile = WorkoutGameGapJumpGeometry::canonicalProfile();

        QVERIFY(profile.ready);
        QVERIFY(WorkoutGameGapJumpGeometry::validate(profile));
        QVERIFY(near(profile.socketHalfWidthMeters, 0.68));
        QVERIFY(near(profile.prepareLeadMeters, 45.0));
        QVERIFY(near(profile.splitLengthMeters, 12.0));
        QVERIFY(near(profile.launchWindowLeadMeters, 10.0));
        QVERIFY(near(profile.lockLeadMeters, 3.0));
        QVERIFY(near(profile.mergeLengthMeters, 18.0));
        QCOMPARE(profile.speedWindowMilliseconds, 500);
        QCOMPARE(profile.powerHoldMilliseconds, 500);
        QVERIFY(profile.prepareLeadMeters > profile.splitLengthMeters);
        QVERIFY(profile.splitLengthMeters > profile.launchWindowLeadMeters);
        QVERIFY(profile.launchWindowLeadMeters > profile.lockLeadMeters);

        const std::array<WorkoutGameGapJumpLine, 3> ids = {
            WorkoutGameGapJumpLine::Short,
            WorkoutGameGapJumpLine::Medium,
            WorkoutGameGapJumpLine::Long
        };
        const std::array<double, 3> laterals = {-2.3, 0.0, 2.3};
        const std::array<double, 3> gaps = {1.8, 3.2, 4.7};
        const std::array<double, 3> thresholds = {4.0, 5.2, 6.6};

        for (std::size_t index = 0; index < profile.lines.size(); ++index) {
            const auto &line = profile.lines[index];
            QCOMPARE(line.id, ids[index]);
            QVERIFY(near(line.lateralMeters, laterals[index]));
            QVERIFY(near(line.gapLengthMeters, gaps[index]));
            QVERIFY(near(line.coldThresholdMetersPerSecond,
                         thresholds[index]));
            QVERIFY(WorkoutGameGapJumpGeometry::line(profile, ids[index])
                    == &profile.lines[index]);
            if (index > 0) {
                QVERIFY(line.lateralMeters
                        > profile.lines[index - 1].lateralMeters);
                QVERIFY(line.gapLengthMeters
                        > profile.lines[index - 1].gapLengthMeters);
                QVERIFY(line.coldThresholdMetersPerSecond
                        > profile.lines[index - 1]
                            .coldThresholdMetersPerSecond);
            }
        }
    }

    void exactColdSpeedBoundariesChooseExpectedLine()
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

    void fiftyMillisecondTimingIsDeterministicAtPromotionAndDemotion()
    {
        for (int run = 0; run < 3; ++run) {
            WorkoutGameGapJumpSelector selector;
            QCOMPARE(selector.update(4.0, FixedStepMilliseconds)
                             .provisionalLine,
                     WorkoutGameGapJumpLine::Short);

            QCOMPARE(holdSpeed(selector, 5.55, 250).provisionalLine,
                     WorkoutGameGapJumpLine::Short);
            QCOMPARE(selector.update(5.55, FixedStepMilliseconds)
                             .provisionalLine,
                     WorkoutGameGapJumpLine::Medium);

            const double belowRelease = std::nextafter(4.85, 0.0);
            QCOMPARE(holdSpeed(selector, belowRelease, 450).provisionalLine,
                     WorkoutGameGapJumpLine::Medium);
            QCOMPARE(selector.update(belowRelease, FixedStepMilliseconds)
                             .provisionalLine,
                     WorkoutGameGapJumpLine::Short);
        }
    }

    void decisionLocksOneLineAndRejectsLateSwitches()
    {
        WorkoutGameGapJumpSelector selector;
        QCOMPARE(selector.update(5.2, FixedStepMilliseconds).provisionalLine,
                 WorkoutGameGapJumpLine::Medium);

        const auto locked = selector.lock(0x1234u, true, true);
        QVERIFY(locked.locked);
        QCOMPARE(locked.actionId, std::uint64_t(0x1234u));
        QCOMPARE(locked.lockedLine, WorkoutGameGapJumpLine::Medium);

        for (int elapsed = 0; elapsed < 2000;
             elapsed += FixedStepMilliseconds) {
            const double lateSpeed = elapsed % 100 == 0 ? 12.0 : 0.0;
            const auto state = selector.update(lateSpeed,
                                               FixedStepMilliseconds);
            QCOMPARE(state.lockedLine, WorkoutGameGapJumpLine::Medium);
            QCOMPARE(state.provisionalLine, WorkoutGameGapJumpLine::Medium);
            QCOMPARE(state.actionId, std::uint64_t(0x1234u));
        }

        const auto relock = selector.lock(0x9999u, false, false);
        QCOMPARE(relock.lockedLine, WorkoutGameGapJumpLine::Medium);
        QCOMPARE(relock.actionId, std::uint64_t(0x1234u));
    }

    void safeBypassIsDistinctAndLocksForEveryFailureMode()
    {
        const auto profile = WorkoutGameGapJumpGeometry::canonicalProfile();
        const double leftmostLine = profile.lines.front().lateralMeters;
        QVERIFY(profile.bypassLateralMeters
                < leftmostLine - 2.0 * profile.socketHalfWidthMeters);
        QVERIFY(WorkoutGameGapJumpGeometry::line(
                    profile, WorkoutGameGapJumpLine::None) == nullptr);

        WorkoutGameGapJumpSelector tooSlow;
        tooSlow.update(std::nextafter(4.0, 0.0), FixedStepMilliseconds);
        QCOMPARE(tooSlow.lock(1u, true, true).lockedLine,
                 WorkoutGameGapJumpLine::None);

        WorkoutGameGapJumpSelector insufficientEffort;
        insufficientEffort.update(6.6, FixedStepMilliseconds);
        QCOMPARE(insufficientEffort.lock(2u, false, true).lockedLine,
                 WorkoutGameGapJumpLine::None);

        WorkoutGameGapJumpSelector staleTelemetry;
        staleTelemetry.update(6.6, FixedStepMilliseconds);
        QCOMPARE(staleTelemetry.lock(3u, true, false).lockedLine,
                 WorkoutGameGapJumpLine::None);

        WorkoutGameGapJumpSelector invalidSpeed;
        invalidSpeed.update(std::numeric_limits<double>::quiet_NaN(),
                            FixedStepMilliseconds);
        QCOMPARE(invalidSpeed.lock(4u, true, true).lockedLine,
                 WorkoutGameGapJumpLine::None);
    }

    void authoredAirtimeIsOrderedAndBoundedPerLine()
    {
        const auto profile = WorkoutGameGapJumpGeometry::canonicalProfile();
        const std::array<double, 3> minimums = {0.25, 0.35, 0.45};
        const std::array<double, 3> maximums = {0.85, 1.10, 1.40};

        QVERIFY(near(profile.maximumFlightSeconds, 2.0));
        for (std::size_t index = 0; index < profile.lines.size(); ++index) {
            const double airtime = profile.lines[index].nominalFlightSeconds;
            QVERIFY(std::isfinite(airtime));
            QVERIFY(airtime >= minimums[index]);
            QVERIFY(airtime <= maximums[index]);
            QVERIFY(airtime < profile.maximumFlightSeconds);
            if (index > 0) {
                QVERIFY(airtime
                        > profile.lines[index - 1].nominalFlightSeconds);
            }
        }
    }

    void mirroredGeometryPreservesSelectionAndGapSemantics()
    {
        const auto canonical = WorkoutGameGapJumpGeometry::canonicalProfile();
        const auto mirrored = WorkoutGameGapJumpGeometry::mirrored(canonical);

        QVERIFY(WorkoutGameGapJumpGeometry::validate(mirrored));
        for (std::size_t index = 0; index < canonical.lines.size(); ++index) {
            QCOMPARE(mirrored.lines[index].id, canonical.lines[index].id);
            QVERIFY(near(mirrored.lines[index].lateralMeters,
                         -canonical.lines[index].lateralMeters));
            QVERIFY(near(mirrored.lines[index].gapLengthMeters,
                         canonical.lines[index].gapLengthMeters));
            QVERIFY(near(mirrored.lines[index].nominalFlightSeconds,
                         canonical.lines[index].nominalFlightSeconds));
        }

        for (double speed : {3.99, 4.0, 5.2, 6.6, 9.0}) {
            QCOMPARE(WorkoutGameGapJumpSelector::coldSelection(
                         canonical, speed),
                     WorkoutGameGapJumpSelector::coldSelection(
                         mirrored, speed));
        }
    }

    void terrainEnumValuesAndSidecarSchemaRemainStable()
    {
        QCOMPARE(int(WorkoutGameTerrainKind::SmoothTrail), 0);
        QCOMPARE(int(WorkoutGameTerrainKind::Roots), 1);
        QCOMPARE(int(WorkoutGameTerrainKind::Rollers), 2);
        QCOMPARE(int(WorkoutGameTerrainKind::Climb), 3);
        QCOMPARE(int(WorkoutGameTerrainKind::RockGarden), 4);
        QCOMPARE(int(WorkoutGameTerrainKind::BunnyHop), 5);
        QCOMPARE(int(WorkoutGameTerrainKind::Drop), 6);
        QCOMPARE(int(WorkoutGameTerrainKind::Skinny), 7);
        QCOMPARE(int(WorkoutGameTerrainKind::Berm), 8);
        QCOMPARE(int(WorkoutGameTerrainKind::LogOver), 9);
        QCOMPARE(int(WorkoutGameTerrainKind::Tabletop), 10);
        QCOMPARE(int(WorkoutGameTerrainKind::RockSlab), 11);
        QCOMPARE(int(WorkoutGameTerrainKind::GapJump), 12);
        QCOMPARE(WorkoutGameCourseDocumentCodec::CurrentSchemaVersion, 4);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameGapJumpIntegration)
#include "testWorkoutGameGapJumpIntegration.moc"
