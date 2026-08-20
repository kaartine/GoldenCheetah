/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameRoadCourse.h"
#include "Train/WorkoutGameRoadProjection.h"

#include <QTest>

#include <cmath>

namespace {

WorkoutGameCourse sampleCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 123u;
    course.durationMs = 30000;
    WorkoutGameSection climb;
    climb.feature = WorkoutGameFeature::Climb;
    climb.terrain = WorkoutGameTerrainKind::Climb;
    climb.durationMs = 15000;
    climb.targetWatts = 220.0;
    climb.gradePercent = 8.0;
    climb.difficulty = 0.7;
    climb.challengeCount = 1;
    climb.visualVariant = 2u;
    WorkoutGameSection log = climb;
    log.feature = WorkoutGameFeature::SprintJump;
    log.terrain = WorkoutGameTerrainKind::LogOver;
    log.startMs = climb.durationMs;
    log.durationMs = 15000;
    log.targetWatts = 260.0;
    log.gradePercent = 0.0;
    log.difficulty = 0.5;
    log.visualVariant = 3u;
    course.sections = {climb, log};
    return course;
}

bool near(double left, double right, double tolerance = 1e-7)
{
    return std::abs(left - right) <= tolerance;
}

}

class TestWorkoutGameRoadCourse : public QObject
{
    Q_OBJECT

private slots:
    void invalidInputsFailClosed()
    {
        QVERIFY(!WorkoutGameRoadCourseBuilder::build({}, 200.0).ready);
        QVERIFY(!WorkoutGameRoadCourseBuilder::build(
                sampleCourse(), 0.0).ready);
        QVERIFY(!WorkoutGameRoadProjection::project({}, 0.0).ready);
    }

    void piecesJoinWithoutPositionHeadingOrWidthGaps()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        QVERIFY(road.ready);
        QVERIFY(road.pieces.size() >= 2u);
        for (std::size_t index = 1; index < road.pieces.size(); ++index) {
            const WorkoutGameRoadConnector &prior = road.pieces[index - 1].exit;
            const WorkoutGameRoadConnector &next = road.pieces[index].entry;
            QVERIFY(near(prior.xMeters, next.xMeters));
            QVERIFY(near(prior.zMeters, next.zMeters));
            QVERIFY(near(prior.elevationMeters, next.elevationMeters));
            QVERIFY(near(prior.headingRadians, next.headingRadians));
            QVERIFY(near(prior.halfWidthMeters, next.halfWidthMeters));
        }
    }

    void logPieceOwnsTimedJumpGate()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        const WorkoutGameRoadPiece *jump = nullptr;
        for (const WorkoutGameRoadPiece &piece : road.pieces) {
            if (piece.challenge.enabled
                    && piece.terrain == WorkoutGameTerrainKind::LogOver) {
                jump = &piece;
                break;
            }
        }
        QVERIFY(jump != nullptr);
        QCOMPARE(jump->animation, WorkoutGameRoadAnimation::Jump);
        QCOMPARE(jump->challenge.profile.cue, WorkoutGameChallengeCue::Jump);
        QVERIFY(jump->challenge.prepareDistanceMeters
                < jump->challenge.decisionDistanceMeters);
        QVERIFY(jump->challenge.decisionDistanceMeters
                - jump->challenge.prepareDistanceMeters <= 6.0 + 1e-9);
        QVERIFY(jump->challenge.decisionDistanceMeters
                < jump->challenge.obstacleDistanceMeters);
        QVERIFY(jump->challenge.obstacleDistanceMeters
                >= jump->startDistanceMeters);
        QVERIFY(jump->challenge.obstacleDistanceMeters
                <= jump->startDistanceMeters + jump->lengthMeters);
    }

    void generationAndSamplingAreDeterministic()
    {
        const WorkoutGameRoadCourse first =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        const WorkoutGameRoadCourse second =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        QCOMPARE(first.pieces.size(), second.pieces.size());
        QCOMPARE(first.totalLengthMeters, second.totalLengthMeters);
        for (double distance = 0.0;
             distance <= first.totalLengthMeters; distance += 3.25) {
            const WorkoutGameRoadSample left =
                    WorkoutGameRoadCourseBuilder::sample(first, distance);
            const WorkoutGameRoadSample right =
                    WorkoutGameRoadCourseBuilder::sample(second, distance);
            QCOMPARE(left.center.xMeters, right.center.xMeters);
            QCOMPARE(left.center.zMeters, right.center.zMeters);
            QCOMPARE(left.center.elevationMeters,
                     right.center.elevationMeters);
        }
    }

    void workoutTimelineIsContinuousAndMonotonic()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        QCOMPARE(road.timeline.size(), std::size_t(2));
        QVERIFY(near(road.timeline[0].endDistanceMeters,
                     road.timeline[1].startDistanceMeters));

        double previousDistance = -1.0;
        for (std::int64_t timeMs = -1000; timeMs <= 31000; timeMs += 17) {
            const WorkoutGameRoadTimelineSample sample =
                    WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(
                            road, timeMs);
            QVERIFY(sample.ready);
            QVERIFY(sample.distanceMeters + 1e-9 >= previousDistance);
            previousDistance = sample.distanceMeters;
        }
        const WorkoutGameRoadTimelineSample boundary =
                WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(road, 15000);
        QCOMPARE(boundary.sourceSectionIndex, std::size_t(1));
        QCOMPARE(boundary.sectionProgress, 0.0);
        QVERIFY(near(boundary.distanceMeters,
                     road.timeline[1].startDistanceMeters));
        const WorkoutGameRoadTimelineSample end =
                WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(road, 40000);
        QCOMPARE(end.distanceMeters, road.totalLengthMeters);
        QCOMPARE(end.sectionProgress, 1.0);
    }

    void climbAndTurnAreVisibleInWorldGeometry()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        const WorkoutGameRoadPiece &first = road.pieces.front();
        QVERIFY(first.exit.elevationMeters > first.entry.elevationMeters);
        QVERIFY(std::abs(first.exit.headingRadians
                - first.entry.headingRadians) > 0.01);
        QVERIFY(std::abs(first.exit.xMeters - first.entry.xMeters) > 0.01);
    }

    void projectionUsesDepthInsteadOfSidewaysTravel()
    {
        WorkoutGameCourse straightCourse = sampleCourse();
        for (WorkoutGameSection &section : straightCourse.sections) {
            section.terrain = WorkoutGameTerrainKind::SmoothTrail;
            section.visualVariant = 0u;
            section.gradePercent = 0.0;
            section.challengeCount = 0;
        }
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(straightCourse, 200.0);
        const WorkoutGameRoadProjectionFrame frame =
                WorkoutGameRoadProjection::project(road, 0.0);
        QVERIFY(frame.ready);
        QVERIFY(frame.slices.size() >= 90u);
        const double center = 1280.0 * 0.5;
        for (const WorkoutGameRoadProjectedSlice &slice : frame.slices) {
            QVERIFY(std::abs(slice.centerX - center) < 1.0);
        }
        QVERIFY(frame.slices.front().centerY
                < frame.slices.back().centerY);
        QVERIFY(frame.slices.front().halfWidthPixels
                < frame.slices.back().halfWidthPixels);
        QCOMPARE(frame.riderScreenX, center);
    }

    void tinyDistanceChangesProduceContinuousProjection()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        const WorkoutGameRoadProjectionFrame first =
                WorkoutGameRoadProjection::project(road, 10.0);
        const WorkoutGameRoadProjectionFrame next =
                WorkoutGameRoadProjection::project(road, 10.01);
        QVERIFY(first.ready);
        QVERIFY(next.ready);
        QCOMPARE(first.slices.size(), next.slices.size());
        for (std::size_t index = 0; index < first.slices.size(); ++index) {
            QVERIFY(std::abs(first.slices[index].centerX
                    - next.slices[index].centerX) < 2.0);
            QVERIFY(std::abs(first.slices[index].centerY
                    - next.slices[index].centerY) < 2.0);
        }
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameRoadCourse)
#include "testWorkoutGameRoadCourse.moc"
