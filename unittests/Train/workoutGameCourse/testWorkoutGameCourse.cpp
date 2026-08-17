/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameCourse.h"

#include <QTest>

#include <cmath>
#include <limits>

namespace {

WorkoutGameInterval interval(
        std::int64_t startMs,
        std::int64_t durationMs,
        double startWatts,
        double endWatts)
{
    return {startMs, durationMs, startWatts, endWatts};
}

}

class TestWorkoutGameCourse : public QObject
{
    Q_OBJECT

private slots:
    void emptyWorkoutProducesExplicitFallbackStatus()
    {
        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build({}, 200.0);

        QCOMPARE(course.status, WorkoutGameCourseStatus::EmptyWorkout);
        QVERIFY(course.sections.empty());
    }

    void invalidFtpIsRejected_data()
    {
        QTest::addColumn<double>("ftp");
        QTest::newRow("zero") << 0.0;
        QTest::newRow("negative") << -1.0;
        QTest::newRow("nan") << std::numeric_limits<double>::quiet_NaN();
    }

    void invalidFtpIsRejected()
    {
        QFETCH(double, ftp);
        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build(
                {interval(0, 60000, 100.0, 100.0)}, ftp);

        QCOMPARE(course.status, WorkoutGameCourseStatus::InvalidFtp);
        QVERIFY(course.sections.empty());
    }

    void malformedTimelineIsRejected_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("does-not-start-at-zero") << 0;
        QTest::newRow("gap") << 1;
        QTest::newRow("zero-duration") << 2;
        QTest::newRow("negative-power") << 3;
        QTest::newRow("non-finite-power") << 4;
    }

    void malformedTimelineIsRejected()
    {
        QFETCH(int, kind);
        std::vector<WorkoutGameInterval> intervals;
        switch (kind) {
        case 0: intervals = {interval(1, 1000, 100.0, 100.0)}; break;
        case 1: intervals = {
                interval(0, 1000, 100.0, 100.0),
                interval(2000, 1000, 100.0, 100.0)}; break;
        case 2: intervals = {interval(0, 0, 100.0, 100.0)}; break;
        case 3: intervals = {interval(0, 1000, -1.0, 100.0)}; break;
        default: intervals = {interval(
                0, 1000, std::numeric_limits<double>::infinity(), 100.0)}; break;
        }

        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build(
                intervals, 200.0);

        QCOMPARE(course.status, WorkoutGameCourseStatus::InvalidInterval);
        QVERIFY(course.sections.empty());
    }

    void workoutShapesMapToExpectedFeatures()
    {
        const std::vector<WorkoutGameInterval> intervals = {
            interval(0, 120000, 80.0, 140.0),
            interval(120000, 10000, 260.0, 260.0),
            interval(130000, 60000, 100.0, 100.0),
            interval(190000, 240000, 190.0, 190.0),
            interval(430000, 120000, 165.0, 165.0),
            interval(550000, 30000, 140.0, 140.0),
            interval(580000, 120000, 140.0, 80.0)
        };

        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build(
                intervals, 200.0, 1234u);

        QCOMPARE(course.status, WorkoutGameCourseStatus::Ready);
        QCOMPARE(course.durationMs, std::int64_t(700000));
        QCOMPARE(course.sections.size(), std::size_t(7));
        QCOMPARE(course.sections[0].feature, WorkoutGameFeature::WarmupTrail);
        QCOMPARE(course.sections[1].feature, WorkoutGameFeature::SprintJump);
        QCOMPARE(course.sections[2].feature, WorkoutGameFeature::RecoveryDescent);
        QCOMPARE(course.sections[3].feature, WorkoutGameFeature::Climb);
        QCOMPARE(course.sections[4].feature, WorkoutGameFeature::FlowTrail);
        QCOMPARE(course.sections[5].feature, WorkoutGameFeature::Trail);
        QCOMPARE(course.sections[6].feature, WorkoutGameFeature::CooldownDescent);
    }

    void recoveryAndCooldownAreGravityAssisted()
    {
        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build({
            interval(0, 60000, 100.0, 100.0),
            interval(60000, 60000, 140.0, 80.0)
        }, 200.0);

        QVERIFY(course.sections[0].gravityAssisted);
        QVERIFY(course.sections[0].gradePercent < 0.0);
        QVERIFY(course.sections[1].gravityAssisted);
        QVERIFY(course.sections[1].gradePercent < 0.0);
    }

    void challengeCountsScaleWithLongSections()
    {
        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build({
            interval(0, 180000, 160.0, 160.0),
            interval(180000, 360000, 190.0, 190.0)
        }, 200.0);

        QCOMPARE(course.sections[0].feature, WorkoutGameFeature::FlowTrail);
        QCOMPARE(course.sections[0].challengeCount, 6);
        QCOMPARE(course.sections[1].feature, WorkoutGameFeature::Climb);
        QCOMPARE(course.sections[1].challengeCount, 6);
    }

    void derivedCourseIsDeterministic()
    {
        const std::vector<WorkoutGameInterval> intervals = {
            interval(0, 60000, 150.0, 150.0),
            interval(60000, 10000, 250.0, 250.0)
        };

        const WorkoutGameCourse first = WorkoutGameCourseBuilder::build(intervals, 200.0);
        const WorkoutGameCourse second = WorkoutGameCourseBuilder::build(intervals, 200.0);

        QCOMPARE(first.seed, second.seed);
        QVERIFY(first.seed != 0u);
        QCOMPARE(first.sections[0].visualVariant, second.sections[0].visualVariant);
        QCOMPARE(first.sections[1].visualVariant, second.sections[1].visualVariant);
    }

    void explicitSeedControlsOnlyVisualVariants()
    {
        const std::vector<WorkoutGameInterval> intervals = {
            interval(0, 60000, 150.0, 150.0),
            interval(60000, 60000, 160.0, 160.0)
        };

        const WorkoutGameCourse first = WorkoutGameCourseBuilder::build(intervals, 200.0, 1u);
        const WorkoutGameCourse second = WorkoutGameCourseBuilder::build(intervals, 200.0, 2u);

        QCOMPARE(first.sections[0].feature, second.sections[0].feature);
        QCOMPARE(first.sections[0].targetWatts, second.sections[0].targetWatts);
        QVERIFY(first.sections[0].visualVariant != second.sections[0].visualVariant
                || first.sections[1].visualVariant != second.sections[1].visualVariant);
    }

    void difficultyAndVariantsStayBounded()
    {
        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build({
            interval(0, 10000, 1500.0, 1500.0)
        }, 100.0, 42u);

        QCOMPARE(course.sections[0].difficulty, 1.0);
        QVERIFY(course.sections[0].visualVariant < 8u);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameCourse)
#include "testWorkoutGameCourse.moc"
