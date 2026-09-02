/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameGapJumpGeometry.h"

#include <QTest>

#include <cmath>
#include <limits>

namespace {

bool near(double left, double right, double tolerance = 1e-12)
{
    return std::abs(left - right) <= tolerance;
}

}

class TestWorkoutGameGapJumpGeometry : public QObject
{
    Q_OBJECT

private slots:
    void canonicalProfileUsesReconciledLineContract()
    {
        const auto profile = WorkoutGameGapJumpGeometry::canonicalProfile();

        QVERIFY(profile.ready);
        QVERIFY(WorkoutGameGapJumpGeometry::validate(profile));
        QCOMPARE(profile.lines[0].id, WorkoutGameGapJumpLine::Short);
        QCOMPARE(profile.lines[1].id, WorkoutGameGapJumpLine::Medium);
        QCOMPARE(profile.lines[2].id, WorkoutGameGapJumpLine::Long);
        QVERIFY(near(profile.lines[0].lateralMeters, -2.3));
        QVERIFY(near(profile.lines[1].lateralMeters, 0.0));
        QVERIFY(near(profile.lines[2].lateralMeters, 2.3));
        QVERIFY(near(profile.lines[0].gapLengthMeters, 1.8));
        QVERIFY(near(profile.lines[1].gapLengthMeters, 3.2));
        QVERIFY(near(profile.lines[2].gapLengthMeters, 4.7));
        QVERIFY(near(profile.lines[0].coldThresholdMetersPerSecond, 4.0));
        QVERIFY(near(profile.lines[1].coldThresholdMetersPerSecond, 5.2));
        QVERIFY(near(profile.lines[2].coldThresholdMetersPerSecond, 6.6));
        QVERIFY(near(profile.lines[0].nominalFlightSeconds, 0.55));
        QVERIFY(near(profile.lines[1].nominalFlightSeconds, 0.78));
        QVERIFY(near(profile.lines[2].nominalFlightSeconds, 1.05));
        QVERIFY(near(profile.hysteresisMetersPerSecond, 0.35));
        QVERIFY(near(profile.maximumFlightSeconds, 2.0));
    }

    void difficultyScalingStaysInsideAuthoredBounds()
    {
        const auto easy = WorkoutGameGapJumpGeometry::profile(0.0);
        const auto hard = WorkoutGameGapJumpGeometry::profile(1.0);
        const auto canonical = WorkoutGameGapJumpGeometry::canonicalProfile();

        QVERIFY(WorkoutGameGapJumpGeometry::validate(easy));
        QVERIFY(WorkoutGameGapJumpGeometry::validate(hard));
        for (std::size_t index = 0; index < canonical.lines.size(); ++index) {
            QVERIFY(near(easy.lines[index].gapLengthMeters,
                         canonical.lines[index].gapLengthMeters * 0.85));
            QVERIFY(near(hard.lines[index].gapLengthMeters,
                         canonical.lines[index].gapLengthMeters * 1.15));
            QVERIFY(near(easy.lines[index].coldThresholdMetersPerSecond,
                         canonical.lines[index].coldThresholdMetersPerSecond
                             * 0.90));
            QVERIFY(near(hard.lines[index].coldThresholdMetersPerSecond,
                         canonical.lines[index].coldThresholdMetersPerSecond
                             * 1.10));
            QVERIFY(easy.lines[index].nominalFlightSeconds >= 0.25);
            QVERIFY(hard.lines[index].nominalFlightSeconds <= 1.40);
            QVERIFY(hard.lines[index].nominalFlightSeconds
                    <= hard.maximumFlightSeconds);
        }
    }

    void malformedProfilesFailClosed()
    {
        auto profile = WorkoutGameGapJumpGeometry::canonicalProfile();
        profile.lines[1].id = WorkoutGameGapJumpLine::Short;
        QVERIFY(!WorkoutGameGapJumpGeometry::validate(profile));

        profile = WorkoutGameGapJumpGeometry::canonicalProfile();
        profile.lines[1].gapLengthMeters = profile.lines[0].gapLengthMeters;
        QVERIFY(!WorkoutGameGapJumpGeometry::validate(profile));

        profile = WorkoutGameGapJumpGeometry::canonicalProfile();
        profile.lines[2].coldThresholdMetersPerSecond =
                profile.lines[1].coldThresholdMetersPerSecond;
        QVERIFY(!WorkoutGameGapJumpGeometry::validate(profile));

        profile = WorkoutGameGapJumpGeometry::canonicalProfile();
        profile.lines[0].lateralMeters =
                std::numeric_limits<double>::infinity();
        QVERIFY(!WorkoutGameGapJumpGeometry::validate(profile));

        profile = WorkoutGameGapJumpGeometry::canonicalProfile();
        profile.lines[1].lateralMeters = profile.lines[0].lateralMeters + 1.0;
        QVERIFY(!WorkoutGameGapJumpGeometry::validate(profile));

        profile = WorkoutGameGapJumpGeometry::canonicalProfile();
        profile.lines[2].nominalFlightSeconds = 2.01;
        QVERIFY(!WorkoutGameGapJumpGeometry::validate(profile));

        QVERIFY(!WorkoutGameGapJumpGeometry::profile(
                    std::numeric_limits<double>::quiet_NaN()).ready);
    }

    void mirroringPreservesStableLineSemantics()
    {
        const auto original = WorkoutGameGapJumpGeometry::canonicalProfile();
        const auto mirrored = WorkoutGameGapJumpGeometry::mirrored(original);

        QVERIFY(WorkoutGameGapJumpGeometry::validate(mirrored));
        for (std::size_t index = 0; index < original.lines.size(); ++index) {
            QCOMPARE(mirrored.lines[index].id, original.lines[index].id);
            QVERIFY(near(mirrored.lines[index].lateralMeters,
                         -original.lines[index].lateralMeters));
            QVERIFY(near(mirrored.lines[index].gapLengthMeters,
                         original.lines[index].gapLengthMeters));
            QVERIFY(near(mirrored.lines[index].coldThresholdMetersPerSecond,
                         original.lines[index].coldThresholdMetersPerSecond));
        }
    }

    void lookupRejectsFallbackAndUnknownLines()
    {
        const auto profile = WorkoutGameGapJumpGeometry::canonicalProfile();
        QVERIFY(WorkoutGameGapJumpGeometry::line(
                    profile, WorkoutGameGapJumpLine::Short) != nullptr);
        QVERIFY(WorkoutGameGapJumpGeometry::line(
                    profile, WorkoutGameGapJumpLine::None) == nullptr);
        QVERIFY(WorkoutGameGapJumpGeometry::line(
                    profile, static_cast<WorkoutGameGapJumpLine>(99))
                == nullptr);

        auto malformed = profile;
        malformed.lines[1].id = WorkoutGameGapJumpLine::Short;
        QVERIFY(WorkoutGameGapJumpGeometry::line(
                    malformed, WorkoutGameGapJumpLine::Short) == nullptr);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameGapJumpGeometry)
#include "testWorkoutGameGapJumpGeometry.moc"
