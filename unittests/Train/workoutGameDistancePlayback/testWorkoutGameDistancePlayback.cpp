/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameDistancePlayback.h"

#include <QTest>

#include <limits>

namespace {

WorkoutGameDistanceCourse sampleCourse()
{
    WorkoutGameDistanceCourse course;
    course.status = WorkoutGameDistanceCourseStatus::Ready;
    course.seed = 9u;
    course.nominalDurationMs = 30000;
    course.totalDistanceMeters = 300.0;
    course.elevationGainMeters = 5.0;

    WorkoutGameDistanceCourseSection first;
    first.feature = WorkoutGameFeature::Climb;
    first.terrain = WorkoutGameTerrainKind::Climb;
    first.nominalDurationMs = 10000;
    first.minimumDurationMs = 9000;
    first.maximumDurationMs = 12500;
    first.lengthMeters = 100.0;
    first.targetStartWatts = 150.0;
    first.targetEndWatts = 250.0;
    first.gradePercent = 5.0;
    first.endElevationMeters = 5.0;
    first.difficulty = 0.7;
    first.visualVariant = 3u;

    WorkoutGameDistanceCourseSection second;
    second.feature = WorkoutGameFeature::RecoveryDescent;
    second.terrain = WorkoutGameTerrainKind::Drop;
    second.sourceStartMs = 10000;
    second.nominalDurationMs = 20000;
    second.minimumDurationMs = 14000;
    second.maximumDurationMs = 30000;
    second.startDistanceMeters = 100.0;
    second.lengthMeters = 200.0;
    second.startElevationMeters = 5.0;
    second.endElevationMeters = -3.0;
    second.targetStartWatts = 100.0;
    second.targetEndWatts = 100.0;
    second.gradePercent = -4.0;
    second.difficulty = 0.2;
    second.visualVariant = 5u;
    second.adjustableConnector = true;

    course.sections = {first, second};
    return course;
}

}

class TestWorkoutGameDistancePlayback : public QObject
{
    Q_OBJECT

private slots:
    void invalidCourseCannotBeConfigured()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(!playback.configure(WorkoutGameDistanceCourse()));
        QVERIFY(!playback.atDistance(0.0).ready);
    }

    void distanceMapsToSectionTimeAndRampedTarget()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        const WorkoutGameDistancePlaybackSnapshot start = playback.atDistance(0.0);
        const WorkoutGameDistancePlaybackSnapshot middle = playback.atDistance(50.0);
        const WorkoutGameDistancePlaybackSnapshot boundary = playback.atDistance(100.0);

        QVERIFY(start.ready);
        QCOMPARE(start.sectionIndex, std::size_t(0));
        QCOMPARE(start.nominalTimeMs, std::int64_t(0));
        QCOMPARE(start.targetWatts, 150.0);
        QCOMPARE(middle.sectionIndex, std::size_t(0));
        QCOMPARE(middle.nominalTimeMs, std::int64_t(5000));
        QCOMPARE(middle.targetWatts, 200.0);
        QCOMPARE(boundary.sectionIndex, std::size_t(1));
        QCOMPARE(boundary.nominalTimeMs, std::int64_t(10000));
        QCOMPARE(boundary.targetWatts, 100.0);
    }

    void distanceIsClampedAndFinishIsExplicit()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        const WorkoutGameDistancePlaybackSnapshot before = playback.atDistance(-10.0);
        const WorkoutGameDistancePlaybackSnapshot finish = playback.atDistance(300.0);
        const WorkoutGameDistancePlaybackSnapshot beyond = playback.atDistance(1000.0);

        QCOMPARE(before.distanceMeters, 0.0);
        QVERIFY(!before.finished);
        QCOMPARE(finish.distanceMeters, 300.0);
        QCOMPARE(finish.nominalTimeMs, std::int64_t(30000));
        QVERIFY(finish.finished);
        QCOMPARE(beyond.distanceMeters, 300.0);
        QVERIFY(beyond.finished);
    }

    void elapsedTimePreventsSkippingMinimumExposure()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        const WorkoutGameDistancePlaybackSnapshot fast =
                playback.atProgress(100.0, 4500);
        QVERIFY(fast.ready);
        QCOMPARE(fast.sectionIndex, std::size_t(0));
        QCOMPARE(fast.distanceMeters, 50.0);
        QCOMPARE(fast.nominalTimeMs, std::int64_t(4500));
        QCOMPARE(fast.targetWatts, 195.0);

        const WorkoutGameDistancePlaybackSnapshot stopped =
                playback.atProgress(100.0, 6250);
        QCOMPARE(stopped.sectionIndex, std::size_t(0));
        QCOMPARE(stopped.distanceMeters, 50.0);

        const WorkoutGameDistancePlaybackSnapshot boundary =
                playback.atProgress(150.0, 10750);
        QCOMPARE(boundary.sectionIndex, std::size_t(1));
        QCOMPARE(boundary.distanceMeters, 100.0);

        const WorkoutGameDistancePlaybackSnapshot stillStopped =
                playback.atProgress(150.0, 30000);
        QCOMPARE(stillStopped.distanceMeters, 100.0);
        QVERIFY(!stillStopped.finished);
    }

    void lateSectionEntryStartsANewExposureWindow()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        QCOMPARE(playback.atProgress(50.0, 20000).distanceMeters, 50.0);
        const WorkoutGameDistancePlaybackSnapshot boundary =
                playback.atProgress(100.0, 21000);
        QCOMPARE(boundary.sectionIndex, std::size_t(1));
        QCOMPARE(boundary.distanceMeters, 100.0);

        const WorkoutGameDistancePlaybackSnapshot next =
                playback.atProgress(150.0, 21100);
        QCOMPARE(next.sectionIndex, std::size_t(1));
        QVERIFY(next.distanceMeters >= 100.0);
        QVERIFY(next.distanceMeters < 102.0);
    }

    void rawDistanceBeyondCourseDoesNotFreezeLaterPedalling()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        const WorkoutGameDistancePlaybackSnapshot early =
                playback.atProgress(1000.0, 1000);
        QVERIFY(early.distanceMeters > 10.0);
        QVERIFY(early.distanceMeters < 12.0);

        const WorkoutGameDistancePlaybackSnapshot later =
                playback.atProgress(1001.0, 2000);
        QVERIFY(later.distanceMeters > early.distanceMeters);
    }

    void rawDistanceResetDoesNotCreatePhantomAdvance()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        QCOMPARE(playback.atProgress(100.0, 4500).distanceMeters, 50.0);
        QCOMPARE(playback.atProgress(80.0, 5000).distanceMeters, 50.0);
        QCOMPARE(playback.atProgress(80.0, 6000).distanceMeters, 50.0);
        QCOMPARE(playback.atProgress(0.0, 7000).distanceMeters, 50.0);
        QCOMPARE(playback.atProgress(0.0, 8000).distanceMeters, 50.0);
    }

    void configureResetsProgressState()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));
        QVERIFY(playback.atProgress(100.0, 4500).distanceMeters > 0.0);

        QVERIFY(playback.configure(sampleCourse()));
        QCOMPARE(playback.atProgress(0.0, 0).distanceMeters, 0.0);
        QCOMPARE(playback.atProgress(0.0, 30000).distanceMeters, 0.0);
    }

    void stationaryTimeDoesNotCountAsWorkoutExposure()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        const WorkoutGameDistancePlaybackSnapshot overdue =
                playback.atProgress(0.0, 13000);
        QCOMPARE(overdue.distanceMeters, 0.0);
        QCOMPARE(overdue.sectionElapsedMs, std::int64_t(0));
        QVERIFY(!overdue.maximumExposureExceeded);

        const WorkoutGameDistancePlaybackSnapshot resumed =
                playback.atProgress(100.0, 14000);
        QVERIFY(resumed.distanceMeters > 11.0);
        QVERIFY(resumed.distanceMeters < 12.0);
        QCOMPARE(resumed.sectionElapsedMs, std::int64_t(1000));
    }

    void maximumExposureReportsExcessActiveRidingOnly()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        const WorkoutGameDistancePlaybackSnapshot overdue =
                playback.atProgress(50.0, 13000);
        QCOMPARE(overdue.sectionElapsedMs, std::int64_t(13000));
        QVERIFY(overdue.maximumExposureExceeded);
    }

    void inactiveDistanceDriftDoesNotOpenTheExposureGate()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        QCOMPARE(playback.atProgress(0.001, 5000, false).distanceMeters, 0.0);
        const WorkoutGameDistancePlaybackSnapshot moving =
                playback.atProgress(100.001, 6000, true);
        QVERIFY(moving.distanceMeters > 11.0);
        QVERIFY(moving.distanceMeters < 12.0);
        QCOMPARE(moving.sectionElapsedMs, std::int64_t(1000));
    }

    void inactiveDistanceDriftCannotUseAnAlreadyOpenGate()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        const WorkoutGameDistancePlaybackSnapshot active =
                playback.atProgress(50.0, 4500, true);
        QCOMPARE(active.distanceMeters, 50.0);
        const WorkoutGameDistancePlaybackSnapshot drift =
                playback.atProgress(50.01, 5000, false);
        QCOMPARE(drift.distanceMeters, active.distanceMeters);
        QCOMPARE(drift.sectionElapsedMs, active.sectionElapsedMs);
    }

    void rampTargetFollowsActiveTimeInsteadOfSlowDistance()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        const WorkoutGameDistancePlaybackSnapshot halfway =
                playback.atProgress(10.0, 5000, true);
        QCOMPARE(halfway.distanceMeters, 10.0);
        QCOMPARE(halfway.nominalTimeMs, std::int64_t(5000));
        QCOMPARE(halfway.targetWatts, 200.0);

        const WorkoutGameDistancePlaybackSnapshot stationary =
                playback.atProgress(10.0, 9000, false);
        QCOMPARE(stationary.nominalTimeMs, halfway.nominalTimeMs);
        QCOMPARE(stationary.targetWatts, halfway.targetWatts);
    }

    void explicitSeekAnchorsRemainingSectionExposure()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        const WorkoutGameDistancePlaybackSnapshot seek =
                playback.seekToDistance(150.0, 20000);
        QCOMPARE(seek.sectionIndex, std::size_t(1));
        QCOMPARE(seek.distanceMeters, 150.0);

        const WorkoutGameDistancePlaybackSnapshot next =
                playback.atProgress(250.0, 21000);
        QVERIFY(next.distanceMeters > 150.0);
        QVERIFY(next.distanceMeters < 165.0);
    }

    void progressRequiresFiniteDistanceAndNonNegativeElapsedTime()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        QVERIFY(!playback.atProgress(
                    std::numeric_limits<double>::quiet_NaN(), 1000).ready);
        QVERIFY(!playback.atProgress(10.0, -1).ready);
    }

    void nonFiniteDistanceFailsClosed()
    {
        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(sampleCourse()));

        const WorkoutGameDistancePlaybackSnapshot result = playback.atDistance(
                std::numeric_limits<double>::quiet_NaN());

        QVERIFY(!result.ready);
    }

    void visualCoursePreservesFeaturesAndNominalTimeline()
    {
        const WorkoutGameCourse visual =
                WorkoutGameDistancePlayback::visualCourse(sampleCourse());

        QCOMPARE(visual.status, WorkoutGameCourseStatus::Ready);
        QCOMPARE(visual.seed, std::uint32_t(9));
        QCOMPARE(visual.durationMs, std::int64_t(30000));
        QCOMPARE(visual.sections.size(), std::size_t(2));
        QCOMPARE(visual.sections[0].startMs, std::int64_t(0));
        QCOMPARE(visual.sections[0].durationMs, std::int64_t(10000));
        QCOMPARE(visual.sections[0].lengthMeters, 100.0);
        QCOMPARE(visual.sections[0].feature, WorkoutGameFeature::Climb);
        QCOMPARE(visual.sections[0].targetWatts, 200.0);
        QCOMPARE(visual.sections[1].startMs, std::int64_t(10000));
        QCOMPARE(visual.sections[1].durationMs, std::int64_t(20000));
        QCOMPARE(visual.sections[1].lengthMeters, 200.0);
        QCOMPARE(visual.sections[1].feature,
                 WorkoutGameFeature::RecoveryDescent);
        QVERIFY(visual.sections[1].gravityAssisted);
    }

    void technicalTrailBecomesAChallengeButRecoveryDoesNot()
    {
        WorkoutGameDistanceCourse course = sampleCourse();
        course.sections[0].feature = WorkoutGameFeature::Trail;
        course.sections[0].terrain = WorkoutGameTerrainKind::Skinny;
        course.sections[1].terrain = WorkoutGameTerrainKind::Berm;

        const WorkoutGameCourse visual =
                WorkoutGameDistancePlayback::visualCourse(course);

        QCOMPARE(visual.sections[0].challengeCount, 1);
        QCOMPARE(visual.sections[1].challengeCount, 0);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameDistancePlayback)
#include "testWorkoutGameDistancePlayback.moc"
