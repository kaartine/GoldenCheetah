/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameSimulation.h"

#include <QTest>

#include <cmath>
#include <limits>

namespace {

WorkoutGameCourse courseFor(
        const std::vector<WorkoutGameInterval> &intervals,
        double ftp = 200.0)
{
    return WorkoutGameCourseBuilder::build(intervals, ftp, 123u);
}

WorkoutGameSimulationInput sample(
        std::int64_t timeMs,
        double watts,
        double target,
        double cadence = 85.0)
{
    WorkoutGameSimulationInput input;
    input.workoutTimeMs = timeMs;
    input.actualWatts = watts;
    input.targetWatts = target;
    input.cadenceRpm = cadence;
    return input;
}

WorkoutGameCourse challengeCourse(WorkoutGameTerrainKind terrain)
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 123u;
    course.durationMs = 10000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::Trail;
    section.terrain = terrain;
    section.durationMs = course.durationMs;
    section.targetWatts = 200.0;
    section.difficulty = 0.5;
    section.challengeCount = 1;
    course.sections.push_back(section);
    return course;
}

}

class TestWorkoutGameSimulation : public QObject
{
    Q_OBJECT

private slots:
    void invalidCourseCannotBeConfigured()
    {
        WorkoutGameSimulation simulation;
        WorkoutGameCourse invalid;

        QVERIFY(!simulation.configure(invalid, 200.0));
        QVERIFY(!simulation.update(sample(0, 0.0, 0.0)).ready);
    }

    void invalidSimulationFtpIsRejected()
    {
        WorkoutGameSimulation simulation;
        const WorkoutGameCourse course = courseFor({{0, 60000, 150.0, 150.0}});

        QVERIFY(!simulation.configure(course, 0.0));
        QVERIFY(!simulation.configure(course, std::numeric_limits<double>::quiet_NaN()));
    }

    void workoutTimeSelectsSectionAndProgress()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(courseFor({
            {0, 60000, 150.0, 150.0},
            {60000, 10000, 250.0, 250.0}
        }), 200.0));

        simulation.update(sample(0, 150.0, 150.0));
        const WorkoutGameSimulationSnapshot result =
                simulation.update(sample(65000, 250.0, 250.0));

        QCOMPARE(result.activeSection, 1);
        QCOMPARE(result.courseProgress, 65000.0 / 70000.0);
        QCOMPARE(result.sectionProgress, 0.5);
        QCOMPARE(result.droppedCatchupMs, std::int64_t(64000));
    }

    void strongSprintTakesMainJumpLine()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(courseFor({{0, 10000, 250.0, 250.0}}), 200.0));

        simulation.update(sample(0, 250.0, 250.0, 90.0));
        const WorkoutGameSimulationSnapshot result =
                simulation.update(sample(7000, 250.0, 250.0, 90.0));

        QCOMPARE(result.featureOutcome, WorkoutGameFeatureOutcome::Completed);
        QCOMPARE(result.route, WorkoutGameRoute::MainLine);
    }

    void weakSprintUsesSafeBypass()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(courseFor({{0, 10000, 250.0, 250.0}}), 200.0));

        simulation.update(sample(0, 100.0, 250.0, 50.0));
        const WorkoutGameSimulationSnapshot result =
                simulation.update(sample(7000, 100.0, 250.0, 50.0));

        QCOMPARE(result.featureOutcome, WorkoutGameFeatureOutcome::Bypassed);
        QCOMPARE(result.route, WorkoutGameRoute::SafeBypass);
    }

    void tabletopRequiresEnoughApproachSpeedAndAwardsBonus()
    {
        WorkoutGameSimulation strong;
        WorkoutGameSimulation slow;
        const WorkoutGameCourse course = challengeCourse(
                WorkoutGameTerrainKind::Tabletop);
        QVERIFY(strong.configure(course, 200.0));
        QVERIFY(slow.configure(course, 200.0));

        WorkoutGameSimulationSnapshot strongResult;
        WorkoutGameSimulationSnapshot slowResult;
        for (std::int64_t time = 0; time <= 7500; time += 500) {
            WorkoutGameSimulationInput strongInput = sample(
                    time, 200.0, 200.0, 85.0);
            strongInput.authoritativeSpeedKph = 20.0;
            strongResult = strong.update(strongInput);
            WorkoutGameSimulationInput slowInput = strongInput;
            slowInput.authoritativeSpeedKph = 5.0;
            slowResult = slow.update(slowInput);
        }

        QCOMPARE(strongResult.featureOutcome,
                 WorkoutGameFeatureOutcome::Completed);
        QCOMPARE(strongResult.challengeReadiness, 1.0);
        QVERIFY(strongResult.score >= strongResult.challenge.bonusPoints);
        QCOMPARE(slowResult.featureOutcome,
                 WorkoutGameFeatureOutcome::Bypassed);
        QVERIFY(slowResult.challengeReadiness < 0.5);
        QVERIFY(strongResult.score > slowResult.score);
        QCOMPARE(strongResult.score - slowResult.score,
                 strongResult.challenge.bonusPoints);
    }

    void activeChallengeReportsReadinessBeforeDecision()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(
                challengeCourse(WorkoutGameTerrainKind::RockGarden), 200.0));
        WorkoutGameSimulationInput input = sample(0, 160.0, 200.0, 60.0);
        input.authoritativeSpeedKph = 6.0;
        simulation.update(input);
        input.workoutTimeMs = 3000;

        const WorkoutGameSimulationSnapshot result = simulation.update(input);

        QCOMPARE(result.featureOutcome, WorkoutGameFeatureOutcome::Active);
        QCOMPARE(result.challenge.cue, WorkoutGameChallengeCue::CarrySpeed);
        QVERIFY(result.challengeReadiness > 0.0);
        QVERIFY(result.challengeReadiness < 1.0);
    }

    void recoveryDescentCarriesSpeedWithoutPower()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(courseFor({{0, 60000, 100.0, 100.0}}), 200.0));

        const WorkoutGameSimulationSnapshot initial =
                simulation.update(sample(0, 0.0, 100.0, 0.0));
        const WorkoutGameSimulationSnapshot result =
                simulation.update(sample(1000, 0.0, 100.0, 0.0));

        QVERIFY(initial.speedKph >= 30.0);
        QVERIFY(result.speedKph >= 30.0);
        QCOMPARE(result.adherence, 1.0);
    }

    void distanceRideUsesAuthoritativeSlopeSpeed()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(
                courseFor({{0, 60000, 180.0, 180.0}}), 200.0));

        WorkoutGameSimulationInput moving = sample(0, 180.0, 180.0);
        moving.authoritativeSpeedKph = 20.0;
        moving.drivetrainSpeedLimitKph = 7.2;
        QCOMPARE(simulation.update(moving).speedKph, 7.2);

        WorkoutGameSimulationInput stopped = sample(0, 180.0, 180.0);
        stopped.authoritativeSpeedKph = 0.0;
        stopped.drivetrainSpeedLimitKph = 7.2;
        QCOMPARE(simulation.update(stopped).speedKph, 0.0);
    }

    void recoveryDescentCanFreewheelFasterThanSelectedGear()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(
                courseFor({{0, 60000, 100.0, 100.0}}), 200.0));

        WorkoutGameSimulationInput input = sample(0, 0.0, 100.0, 0.0);
        input.authoritativeSpeedKph = 34.0;
        input.drivetrainSpeedLimitKph = 7.2;

        QCOMPARE(simulation.update(input).speedKph, 34.0);
    }

    void invalidAuthoritativeSpeedFallsBackToPowerEstimate()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(
                courseFor({{0, 60000, 180.0, 180.0}}), 200.0));

        WorkoutGameSimulationInput input = sample(0, 180.0, 180.0);
        input.authoritativeSpeedKph =
                std::numeric_limits<double>::quiet_NaN();

        QVERIFY(simulation.update(input).speedKph > 8.0);
    }

    void accurateRidingScoresMoreThanMissingTarget()
    {
        const WorkoutGameCourse course = courseFor({{0, 60000, 180.0, 180.0}});
        WorkoutGameSimulation accurate;
        WorkoutGameSimulation missed;
        QVERIFY(accurate.configure(course, 200.0));
        QVERIFY(missed.configure(course, 200.0));
        accurate.update(sample(0, 180.0, 180.0));
        missed.update(sample(0, 0.0, 180.0));

        const WorkoutGameSimulationSnapshot accurateResult =
                accurate.update(sample(1000, 180.0, 180.0));
        const WorkoutGameSimulationSnapshot missedResult =
                missed.update(sample(1000, 0.0, 180.0));

        QVERIFY(accurateResult.score > missedResult.score);
        QVERIFY(accurateResult.streakSeconds > missedResult.streakSeconds);
    }

    void pauseDoesNotAwardScoreOrCatchUpLater()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(courseFor({{0, 60000, 180.0, 180.0}}), 200.0));
        simulation.update(sample(0, 180.0, 180.0));

        WorkoutGameSimulationInput paused = sample(5000, 180.0, 180.0);
        paused.paused = true;
        const WorkoutGameSimulationSnapshot pausedResult = simulation.update(paused);
        const WorkoutGameSimulationSnapshot resumed =
                simulation.update(sample(6000, 180.0, 180.0));

        QCOMPARE(pausedResult.score, std::uint64_t(0));
        QCOMPARE(resumed.droppedCatchupMs, std::int64_t(0));
        QVERIFY(resumed.score > 0u);
    }

    void catchupWorkIsBoundedToOneSecond()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(courseFor({{0, 60000, 180.0, 180.0}}), 200.0));
        simulation.update(sample(0, 180.0, 180.0));

        const WorkoutGameSimulationSnapshot result =
                simulation.update(sample(10000, 180.0, 180.0));

        QCOMPARE(result.droppedCatchupMs, std::int64_t(9000));
        QVERIFY(result.score >= 100u);
        QVERIFY(result.score < 200u);
    }

    void backwardsWorkoutTimeResetsSessionState()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(courseFor({{0, 60000, 180.0, 180.0}}), 200.0));
        simulation.update(sample(0, 180.0, 180.0));
        QVERIFY(simulation.update(sample(1000, 180.0, 180.0)).score > 0u);

        const WorkoutGameSimulationSnapshot reset =
                simulation.update(sample(500, 180.0, 180.0));

        QCOMPARE(reset.score, std::uint64_t(0));
        QCOMPARE(reset.streakSeconds, 0.0);
    }

    void invalidTelemetryIsSanitized()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(courseFor({{0, 60000, 180.0, 180.0}}), 200.0));
        const double nan = std::numeric_limits<double>::quiet_NaN();
        simulation.update(sample(0, nan, nan, nan));

        const WorkoutGameSimulationSnapshot result =
                simulation.update(sample(1000, nan, nan, nan));

        QVERIFY(std::isfinite(result.speedKph));
        QVERIFY(std::isfinite(result.adherence));
        QCOMPARE(result.score, std::uint64_t(0));
    }

    void finishingWorkoutFinalizesActiveChallenge()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(courseFor({{0, 60000, 180.0, 180.0}}), 200.0));
        simulation.update(sample(0, 180.0, 180.0));
        for (std::int64_t time = 1000; time <= 60000; time += 1000) {
            simulation.update(sample(time, 180.0, 180.0));
        }

        const WorkoutGameSimulationSnapshot result =
                simulation.update(sample(60000, 180.0, 180.0));

        QVERIFY(result.finished);
        QCOMPARE(result.activeSection, -1);
        QCOMPARE(simulation.sectionOutcomes()[0],
                 WorkoutGameFeatureOutcome::Completed);
    }

    void sameInputsProduceSameSnapshot()
    {
        const WorkoutGameCourse course = courseFor({{0, 60000, 180.0, 180.0}});
        WorkoutGameSimulation first;
        WorkoutGameSimulation second;
        QVERIFY(first.configure(course, 200.0));
        QVERIFY(second.configure(course, 200.0));

        for (std::int64_t time = 0; time <= 5000; time += 250) {
            const WorkoutGameSimulationInput input = sample(time, 175.0, 180.0, 82.0);
            const WorkoutGameSimulationSnapshot a = first.update(input);
            const WorkoutGameSimulationSnapshot b = second.update(input);
            QCOMPARE(a.score, b.score);
            QCOMPARE(a.speedKph, b.speedKph);
            QCOMPARE(a.streakSeconds, b.streakSeconds);
            QCOMPARE(a.featureOutcome, b.featureOutcome);
        }
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameSimulation)
#include "testWorkoutGameSimulation.moc"
