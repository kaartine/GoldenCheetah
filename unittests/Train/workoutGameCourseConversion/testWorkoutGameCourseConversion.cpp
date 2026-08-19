/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameCourseConversion.h"

#include <QTest>

#include <cmath>

namespace {

void appendInterval(
        std::vector<WorkoutGameInterval> &intervals,
        std::int64_t durationMs,
        double watts)
{
    const std::int64_t startMs = intervals.empty()
            ? 0
            : intervals.back().startMs + intervals.back().durationMs;
    intervals.push_back({startMs, durationMs, watts, watts});
}

std::vector<WorkoutGameInterval> sampleWorkout()
{
    std::vector<WorkoutGameInterval> intervals;
    appendInterval(intervals, 10 * 60000, 140.0);
    for (int repetition = 0; repetition < 3; ++repetition) {
        appendInterval(intervals, 4 * 60000, 205.0 + repetition * 3.0);
        appendInterval(intervals, 10000, 250.0 + repetition * 5.0);
        appendInterval(intervals, 3 * 60000, 110.0);
    }
    appendInterval(intervals, 5 * 60000, 100.0);
    return intervals;
}

}

class TestWorkoutGameCourseConversion : public QObject
{
    Q_OBJECT

private slots:
    void invalidInputIsRejected()
    {
        WorkoutGameCourseConversionRequest request;
        request.intervals = sampleWorkout();
        request.ftpWatts = 0.0;

        const WorkoutGameCourseConversionResult result =
                WorkoutGameCourseConverter::convert(request);

        QCOMPARE(result.status, WorkoutGameCourseConversionStatus::InvalidInput);
        QVERIFY(result.course.sections.empty());
    }

    void balancedPresetProducesCompletePreviewSummary()
    {
        WorkoutGameCourseConversionRequest request;
        request.intervals = sampleWorkout();
        request.ftpWatts = 190.0;
        request.preset = WorkoutGameCoursePreset::Balanced;
        request.seed = 42u;

        const WorkoutGameCourseConversionResult result =
                WorkoutGameCourseConverter::convert(request);

        QCOMPARE(result.status, WorkoutGameCourseConversionStatus::Ready);
        QCOMPARE(result.course.status, WorkoutGameDistanceCourseStatus::Ready);
        QCOMPARE(result.course.seed, std::uint32_t(42));
        QCOMPARE(result.summary.nominalDurationMs,
                 std::int64_t(36 * 60000 + 30000));
        QVERIFY(result.summary.distanceMeters > 5000.0);
        QVERIFY(result.summary.elevationGainMeters > 0.0);
        QVERIFY(result.summary.nominalEstimate.finished);
        QVERIFY(result.summary.fastEstimate.finished);
        QVERIFY(result.summary.slowEstimate.finished);
        QVERIFY(result.summary.fastEstimate.elapsedTimeMs
                < result.summary.nominalEstimate.elapsedTimeMs);
        QVERIFY(result.summary.nominalEstimate.elapsedTimeMs
                < result.summary.slowEstimate.elapsedTimeMs);
        QCOMPARE(result.summary.climbCount, 3);
        QCOMPARE(result.summary.jumpCount, 3);
        QCOMPARE(result.summary.descentCount, 4);
    }

    void presetsChangeTerrainWithoutChangingWorkoutTargets()
    {
        WorkoutGameCourseConversionRequest request;
        request.intervals = sampleWorkout();
        request.ftpWatts = 190.0;
        request.seed = 77u;

        request.preset = WorkoutGameCoursePreset::WorkoutFirst;
        const WorkoutGameCourseConversionResult workoutFirst =
                WorkoutGameCourseConverter::convert(request);
        request.preset = WorkoutGameCoursePreset::Balanced;
        const WorkoutGameCourseConversionResult balanced =
                WorkoutGameCourseConverter::convert(request);
        request.preset = WorkoutGameCoursePreset::RideFirst;
        const WorkoutGameCourseConversionResult rideFirst =
                WorkoutGameCourseConverter::convert(request);

        QCOMPARE(workoutFirst.status, WorkoutGameCourseConversionStatus::Ready);
        QCOMPARE(balanced.status, WorkoutGameCourseConversionStatus::Ready);
        QCOMPARE(rideFirst.status, WorkoutGameCourseConversionStatus::Ready);
        QCOMPARE(workoutFirst.course.sections.size(), balanced.course.sections.size());
        QCOMPARE(balanced.course.sections.size(), rideFirst.course.sections.size());
        QVERIFY(workoutFirst.summary.elevationGainMeters
                < balanced.summary.elevationGainMeters);
        QVERIFY(balanced.summary.elevationGainMeters
                < rideFirst.summary.elevationGainMeters);

        for (std::size_t index = 0; index < balanced.course.sections.size(); ++index) {
            QCOMPARE(workoutFirst.course.sections[index].targetStartWatts,
                     balanced.course.sections[index].targetStartWatts);
            QCOMPARE(balanced.course.sections[index].targetStartWatts,
                     rideFirst.course.sections[index].targetStartWatts);
            QVERIFY(workoutFirst.course.sections[index].minimumDurationMs
                    >= rideFirst.course.sections[index].minimumDurationMs);
            QVERIFY(workoutFirst.course.sections[index].maximumDurationMs
                    <= rideFirst.course.sections[index].maximumDurationMs);
        }
    }

    void sameRequestProducesIdenticalResult()
    {
        WorkoutGameCourseConversionRequest request;
        request.intervals = sampleWorkout();
        request.ftpWatts = 190.0;
        request.preset = WorkoutGameCoursePreset::Balanced;

        const WorkoutGameCourseConversionResult first =
                WorkoutGameCourseConverter::convert(request);
        const WorkoutGameCourseConversionResult second =
                WorkoutGameCourseConverter::convert(request);

        QCOMPARE(first.course.seed, second.course.seed);
        QCOMPARE(first.summary.distanceMeters, second.summary.distanceMeters);
        QCOMPARE(first.summary.nominalEstimate.elapsedTimeMs,
                 second.summary.nominalEstimate.elapsedTimeMs);
        QCOMPARE(first.course.sections.size(), second.course.sections.size());
        for (std::size_t index = 0; index < first.course.sections.size(); ++index) {
            QCOMPARE(first.course.sections[index].lengthMeters,
                     second.course.sections[index].lengthMeters);
            QCOMPARE(first.course.sections[index].gradePercent,
                     second.course.sections[index].gradePercent);
        }
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameCourseConversion)
#include "testWorkoutGameCourseConversion.moc"
