/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameDistanceCourse.h"

#include <QTest>

#include <cmath>
#include <limits>
#include <set>

namespace {

void appendInterval(
        std::vector<WorkoutGameInterval> &intervals,
        std::int64_t durationMs,
        double startWatts,
        double endWatts)
{
    const std::int64_t startMs = intervals.empty()
            ? 0
            : intervals.back().startMs + intervals.back().durationMs;
    intervals.push_back({startMs, durationMs, startWatts, endWatts});
}

std::vector<WorkoutGameInterval> fiveByFourWorkout()
{
    std::vector<WorkoutGameInterval> intervals;
    appendInterval(intervals, 18 * 60000, 100.0, 175.0);
    appendInterval(intervals, 60000, 110.0, 110.0);
    const double efforts[] = {205.0, 207.0, 210.0, 210.0, 212.0};
    const double kicks[] = {235.0, 240.0, 245.0, 250.0, 255.0};
    for (int repetition = 0; repetition < 5; ++repetition) {
        appendInterval(intervals, 230000, efforts[repetition], efforts[repetition]);
        appendInterval(intervals, 10000, kicks[repetition], kicks[repetition]);
        appendInterval(intervals, 180000,
                       repetition == 4 ? 120.0 : 115.0,
                       repetition == 4 ? 120.0 : 115.0);
    }
    appendInterval(intervals, 6 * 60000, 120.0, 90.0);
    return intervals;
}

}

class TestWorkoutGameDistanceCourse : public QObject
{
    Q_OBJECT

private slots:
    void invalidInputsAreRejected()
    {
        const WorkoutGameDistanceCourseGenerationParameters defaults;
        QCOMPARE(WorkoutGameDistanceCourseBuilder::build({}, 190.0).status,
                 WorkoutGameDistanceCourseStatus::EmptyWorkout);
        QCOMPARE(WorkoutGameDistanceCourseBuilder::build(
                    {{0, 60000, 150.0, 150.0}}, 0.0).status,
                 WorkoutGameDistanceCourseStatus::InvalidFtp);

        WorkoutGameDistanceCourseGenerationParameters invalid = defaults;
        invalid.roadPhysics.totalMassKg = 0.0;
        QCOMPARE(WorkoutGameDistanceCourseBuilder::build(
                    {{0, 60000, 150.0, 150.0}}, 190.0, invalid).status,
                 WorkoutGameDistanceCourseStatus::InvalidParameters);
    }

    void excessiveWorkoutIsRejectedBeforeSimulation()
    {
        WorkoutGameDistanceCourseGenerationParameters parameters;
        const std::int64_t excessive = parameters.maximumWorkoutDurationMs + 1;

        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    {{0, excessive, 150.0, 150.0}}, 190.0, parameters);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::ResourceLimit);
        QVERIFY(course.sections.empty());
    }

    void fiveByFourWorkoutProducesACompleteMtbCourse()
    {
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    fiveByFourWorkout(), 190.0,
                    WorkoutGameDistanceCourseGenerationParameters(), 123u);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        QCOMPARE(course.seed, std::uint32_t(123));
        QCOMPARE(course.nominalDurationMs, std::int64_t(60 * 60000));
        QCOMPARE(course.sections.size(), std::size_t(18));
        QVERIFY(course.totalDistanceMeters > 10000.0);
        QVERIFY(course.totalDistanceMeters < 30000.0);
        QVERIFY(course.elevationGainMeters > 100.0);
        QVERIFY(course.elevationLossMeters > 100.0);

        int climbs = 0;
        int jumps = 0;
        int recoveries = 0;
        for (const WorkoutGameDistanceCourseSection &section : course.sections) {
            QVERIFY(section.lengthMeters > 0.0);
            QVERIFY(section.maximumDurationMs >= section.nominalDurationMs);
            QVERIFY(section.minimumDurationMs <= section.nominalDurationMs);
            climbs += section.feature == WorkoutGameFeature::Climb ? 1 : 0;
            jumps += section.feature == WorkoutGameFeature::SprintJump ? 1 : 0;
            recoveries += section.feature
                    == WorkoutGameFeature::RecoveryDescent ? 1 : 0;
        }
        QCOMPARE(climbs, 5);
        QCOMPARE(jumps, 5);
        QCOMPARE(recoveries, 6);
    }

    void recoveryJustAboveSixtyPercentFtpBecomesDescent()
    {
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build({
                    {0, 60000, 140.0, 140.0},
                    {60000, 180000, 115.0, 115.0},
                    {240000, 60000, 140.0, 140.0}
                }, 190.0);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        QCOMPARE(course.sections[1].feature,
                 WorkoutGameFeature::RecoveryDescent);
        QVERIFY(course.sections[1].gradePercent < 0.0);
        QVERIFY(course.sections[1].adjustableConnector);
    }

    void thirtySecondAnaerobicEffortBecomesJumpFeature()
    {
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build({
                    {0, 60000, 120.0, 120.0},
                    {60000, 30000, 250.0, 250.0},
                    {90000, 60000, 110.0, 110.0}
                }, 190.0);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        QCOMPARE(course.sections[1].feature,
                 WorkoutGameFeature::SprintJump);
        QVERIFY(course.sections[1].terrain
                    == WorkoutGameTerrainKind::BunnyHop
                || course.sections[1].terrain
                    == WorkoutGameTerrainKind::LogOver
                || course.sections[1].terrain
                    == WorkoutGameTerrainKind::Tabletop);
    }

    void onlyHighTechnicalitySelectsGapJumpDeterministically()
    {
        const std::vector<WorkoutGameInterval> workout = {
            {0, 60000, 120.0, 120.0},
            {60000, 30000, 250.0, 250.0},
            {90000, 60000, 110.0, 110.0}
        };
        WorkoutGameDistanceCourseGenerationParameters parameters;

        std::uint32_t gapSeed = 0u;
        WorkoutGameDistanceCourse rideFirst;
        parameters.technicality = 0.95;
        for (std::uint32_t seed = 1u; seed <= 64u; ++seed) {
            const WorkoutGameDistanceCourse candidate =
                    WorkoutGameDistanceCourseBuilder::build(
                        workout, 190.0, parameters, seed);
            if (candidate.status == WorkoutGameDistanceCourseStatus::Ready
                    && candidate.sections[1].terrain
                        == WorkoutGameTerrainKind::GapJump) {
                gapSeed = seed;
                rideFirst = candidate;
                break;
            }
        }
        QVERIFY(gapSeed != 0u);

        parameters.technicality = 0.15;
        const WorkoutGameDistanceCourse workoutFirst =
                WorkoutGameDistanceCourseBuilder::build(
                    workout, 190.0, parameters, gapSeed);
        parameters.technicality = 0.55;
        const WorkoutGameDistanceCourse balanced =
                WorkoutGameDistanceCourseBuilder::build(
                    workout, 190.0, parameters, gapSeed);
        parameters.technicality = 0.95;
        const WorkoutGameDistanceCourse repeated =
                WorkoutGameDistanceCourseBuilder::build(
                    workout, 190.0, parameters, gapSeed);

        QCOMPARE(workoutFirst.sections[1].feature,
                 WorkoutGameFeature::SprintJump);
        QCOMPARE(balanced.sections[1].feature,
                 WorkoutGameFeature::SprintJump);
        QCOMPARE(rideFirst.sections[1].feature,
                 WorkoutGameFeature::SprintJump);
        QVERIFY(workoutFirst.sections[1].terrain
                != WorkoutGameTerrainKind::GapJump);
        QVERIFY(balanced.sections[1].terrain
                != WorkoutGameTerrainKind::GapJump);
        QCOMPARE(rideFirst.sections[1].terrain,
                 WorkoutGameTerrainKind::GapJump);
        QCOMPARE(repeated.sections[1].terrain,
                 rideFirst.sections[1].terrain);
        QCOMPARE(repeated.sections[1].visualVariant,
                 rideFirst.sections[1].visualVariant);
    }

    void targetPowerReplaysCloseToNominalDuration()
    {
        const WorkoutGameDistanceCourseGenerationParameters parameters;
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    fiveByFourWorkout(), 190.0, parameters, 42u);

        const WorkoutGameDistanceCourseEstimate estimate =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics);

        QVERIFY(estimate.finished);
        QVERIFY(std::abs(estimate.elapsedTimeMs - course.nominalDurationMs)
                <= 90000);
        QVERIFY(estimate.distanceMeters >= course.totalDistanceMeters);
    }

    void powerChangesCompletionTime()
    {
        const WorkoutGameDistanceCourseGenerationParameters parameters;
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    fiveByFourWorkout(), 190.0, parameters, 77u);

        const WorkoutGameDistanceCourseEstimate slower =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics, 0.85);
        const WorkoutGameDistanceCourseEstimate nominal =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics, 1.0);
        const WorkoutGameDistanceCourseEstimate faster =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics, 1.15);

        QVERIFY(slower.finished);
        QVERIFY(nominal.finished);
        QVERIFY(faster.finished);
        QVERIFY(slower.elapsedTimeMs > nominal.elapsedTimeMs);
        QVERIFY(nominal.elapsedTimeMs > faster.elapsedTimeMs);
    }

    void estimateAppliesMinimumExposurePerSection()
    {
        const WorkoutGameDistanceCourseGenerationParameters parameters;
        WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build({
                    {0, 60000, 300.0, 300.0},
                    {60000, 60000, 300.0, 300.0}
                }, 190.0, parameters, 91u);
        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);

        course.sections[0].startDistanceMeters = 0.0;
        course.sections[0].lengthMeters = 600.0;
        course.sections[0].gradePercent = 0.0;
        course.sections[0].startElevationMeters = 0.0;
        course.sections[0].endElevationMeters = 0.0;
        course.sections[0].minimumDurationMs = 1000;
        course.sections[1].startDistanceMeters = 600.0;
        course.sections[1].lengthMeters = 5.0;
        course.sections[1].gradePercent = 0.0;
        course.sections[1].startElevationMeters = 0.0;
        course.sections[1].endElevationMeters = 0.0;
        course.sections[1].minimumDurationMs = 60000;
        course.totalDistanceMeters = 605.0;
        course.elevationGainMeters = 0.0;
        course.elevationLossMeters = 0.0;
        QVERIFY(WorkoutGameDistanceCourseBuilder::validCourse(course));

        WorkoutGameDistanceCourse unrestricted = course;
        unrestricted.sections[1].minimumDurationMs = 1000;
        const WorkoutGameDistanceCourseEstimate baseline =
                WorkoutGameDistanceCourseEstimator::estimate(
                    unrestricted, parameters.roadPhysics, 1.0, 100);
        const WorkoutGameDistanceCourseEstimate constrained =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics, 1.0, 100);

        QVERIFY(baseline.finished);
        QVERIFY(constrained.finished);
        QVERIFY(constrained.elapsedTimeMs
                >= baseline.elapsedTimeMs + 50000);
    }

    void estimateBootstrapsARampThatStartsAtZeroPower()
    {
        WorkoutGameDistanceCourseGenerationParameters parameters;
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build({
                    {0, 60000, 0.0, 200.0}
                }, 190.0, parameters, 117u);
        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);

        const WorkoutGameDistanceCourseEstimate estimate =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics, 1.0, 100);
        QVERIFY(estimate.finished);
        QVERIFY(estimate.elapsedTimeMs >= course.nominalDurationMs);
    }

    void estimateUsesTheSameMaximumSectionExposureAsRuntime()
    {
        WorkoutGameDistanceCourseGenerationParameters parameters;
        WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build({
                    {0, 10000, 150.0, 150.0}
                }, 190.0, parameters, 118u);
        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        course.sections[0].gradePercent = 0.0;
        course.sections[0].startElevationMeters = 0.0;
        course.sections[0].endElevationMeters = 0.0;
        course.sections[0].lengthMeters = 100000.0;
        course.sections[0].minimumDurationMs = 10000;
        course.sections[0].maximumDurationMs = 10000;
        course.totalDistanceMeters = 100000.0;
        course.elevationGainMeters = 0.0;
        course.elevationLossMeters = 0.0;
        QVERIFY(WorkoutGameDistanceCourseBuilder::validCourse(course));

        const WorkoutGameDistanceCourseEstimate estimate =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics, 1.0, 100);
        QVERIFY(estimate.finished);
        QCOMPARE(estimate.elapsedTimeMs, std::int64_t(10000));
        QCOMPARE(estimate.distanceMeters, course.totalDistanceMeters);
    }

    void sameInputProducesTheSameCourse()
    {
        const WorkoutGameDistanceCourse first =
                WorkoutGameDistanceCourseBuilder::build(
                    fiveByFourWorkout(), 190.0,
                    WorkoutGameDistanceCourseGenerationParameters(), 99u);
        const WorkoutGameDistanceCourse second =
                WorkoutGameDistanceCourseBuilder::build(
                    fiveByFourWorkout(), 190.0,
                    WorkoutGameDistanceCourseGenerationParameters(), 99u);

        QCOMPARE(first.totalDistanceMeters, second.totalDistanceMeters);
        QCOMPARE(first.elevationGainMeters, second.elevationGainMeters);
        QCOMPARE(first.sections.size(), second.sections.size());
        for (std::size_t index = 0; index < first.sections.size(); ++index) {
            QCOMPARE(first.sections[index].lengthMeters,
                     second.sections[index].lengthMeters);
            QCOMPARE(first.sections[index].gradePercent,
                     second.sections[index].gradePercent);
            QCOMPARE(first.sections[index].feature,
                     second.sections[index].feature);
        }
    }

    void balancedCourseUsesAVariedTechnicalPalette()
    {
        std::vector<WorkoutGameInterval> intervals;
        for (int index = 0; index < 14; ++index) {
            appendInterval(intervals, 45000,
                    index % 3 == 0 ? 175.0 : 155.0,
                    index % 3 == 0 ? 175.0 : 155.0);
        }
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    intervals, 190.0,
                    WorkoutGameDistanceCourseGenerationParameters(), 55u);
        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);

        std::set<WorkoutGameTerrainKind> terrains;
        int challenges = 0;
        for (const WorkoutGameDistanceCourseSection &section : course.sections) {
            terrains.insert(section.terrain);
            challenges += section.terrain == WorkoutGameTerrainKind::LogOver
                    || section.terrain == WorkoutGameTerrainKind::Skinny
                    || section.terrain == WorkoutGameTerrainKind::RockGarden;
        }
        QVERIFY(terrains.size() >= std::size_t(4));
        QVERIFY(challenges >= 3);
    }

    void longRecoverySectionsUseGentleDescents()
    {
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build({
                    {0, 60000, 140.0, 140.0},
                    {60000, 60000, 225.0, 225.0},
                    {120000, 10 * 60000, 100.0, 100.0},
                    {720000, 5 * 60000, 110.0, 80.0}
                }, 190.0);
        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        QCOMPARE(course.sections[2].feature,
                 WorkoutGameFeature::RecoveryDescent);
        QVERIFY(course.sections[2].gradePercent > -2.0);
        QCOMPARE(course.sections[3].feature,
                 WorkoutGameFeature::CooldownDescent);
        QVERIFY(course.sections[3].gradePercent > -2.0);
        QVERIFY(course.elevationLossMeters < course.totalDistanceMeters * 0.025);
    }

    void estimatorRejectsMalformedCourse()
    {
        WorkoutGameDistanceCourse malformed;
        malformed.status = WorkoutGameDistanceCourseStatus::Ready;
        malformed.nominalDurationMs = 60000;
        malformed.totalDistanceMeters = 100.0;
        WorkoutGameDistanceCourseSection section;
        section.lengthMeters = std::numeric_limits<double>::quiet_NaN();
        malformed.sections.push_back(section);

        const WorkoutGameDistanceCourseEstimate result =
                WorkoutGameDistanceCourseEstimator::estimate(
                    malformed, WorkoutGameRoadPhysicsParameters());

        QVERIFY(!result.finished);
        QCOMPARE(result.elapsedTimeMs, std::int64_t(0));
    }

    void estimatorAcceptsSerializedDistanceRounding()
    {
        const WorkoutGameDistanceCourseGenerationParameters parameters;
        WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    fiveByFourWorkout(), 190.0, parameters, 101u);
        QVERIFY(course.sections.size() > 1);
        course.sections[1].startDistanceMeters += 0.0005;

        const WorkoutGameDistanceCourseEstimate result =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics);

        QVERIFY(result.finished);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameDistanceCourse)
#include "testWorkoutGameDistanceCourse.moc"
