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
        QCOMPARE(visual.sections[0].feature, WorkoutGameFeature::Climb);
        QCOMPARE(visual.sections[0].targetWatts, 200.0);
        QCOMPARE(visual.sections[1].startMs, std::int64_t(10000));
        QCOMPARE(visual.sections[1].durationMs, std::int64_t(20000));
        QCOMPARE(visual.sections[1].feature,
                 WorkoutGameFeature::RecoveryDescent);
        QVERIFY(visual.sections[1].gravityAssisted);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameDistancePlayback)
#include "testWorkoutGameDistancePlayback.moc"
