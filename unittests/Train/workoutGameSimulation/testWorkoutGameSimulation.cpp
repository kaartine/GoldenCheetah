/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameSimulation.h"
#include "Train/WorkoutGameRoadCourse.h"
#include "Train/WorkoutGameRoadPhysics.h"
#include "Train/VirtualDrivetrain.h"

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

WorkoutGameCourse rideCourse(double gradePercent, bool gravityAssisted = false)
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 123u;
    course.durationMs = 120000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::Trail;
    section.terrain = WorkoutGameTerrainKind::SmoothTrail;
    section.durationMs = course.durationMs;
    section.targetWatts = 200.0;
    section.gradePercent = gradePercent;
    section.gravityAssisted = gravityAssisted;
    course.sections.push_back(section);
    return course;
}

struct GearTrace
{
    double finalSpeedKph = 0.0;
    double maximumStepKph = 0.0;
};

GearTrace rideTrace(int gear, double gradePercent, bool gravityAssisted)
{
    constexpr std::int64_t StepMs = 50;
    constexpr std::int64_t DurationMs = 60000;
    constexpr double PowerWatts = 200.0;
    constexpr double CadenceRpm = 85.0;
    constexpr double WheelCircumferenceMeters = 2.105;

    WorkoutGameRoadPhysics physics;
    WorkoutGameSimulation simulation;
    VirtualDrivetrain drivetrain(gear);
    const bool physicsReady = physics.configure(
            WorkoutGameRoadPhysicsParameters());
    const bool simulationReady = simulation.configure(
            rideCourse(gradePercent, gravityAssisted), PowerWatts);
    Q_ASSERT(physicsReady);
    Q_ASSERT(simulationReady);
    if (!physicsReady || !simulationReady) return {};

    GearTrace trace;
    double previousSpeedKph = 0.0;
    for (std::int64_t timeMs = 0; timeMs <= DurationMs; timeMs += StepMs) {
        const WorkoutGameRoadPhysicsSnapshot road = physics.update(
                {PowerWatts, gradePercent, 0.0},
                timeMs == 0 ? 0 : StepMs);
        WorkoutGameSimulationInput input = sample(
                timeMs, PowerWatts, PowerWatts, CadenceRpm);
        input.authoritativeSpeedKph = road.speedMetersPerSecond * 3.6;
        input.drivetrainSpeedLimitKph = drivetrain.speedKph(
                CadenceRpm, WheelCircumferenceMeters);
        input.virtualGear = gear;
        const WorkoutGameSimulationSnapshot frame = simulation.update(input);
        if (timeMs > 0) {
            trace.maximumStepKph = std::max(
                    trace.maximumStepKph,
                    std::abs(frame.speedKph - previousSpeedKph));
        }
        previousSpeedKph = frame.speedKph;
        trace.finalSpeedKph = frame.speedKph;
    }
    return trace;
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
                simulation.update(sample(7500, 250.0, 250.0, 90.0));

        QCOMPARE(result.featureOutcome, WorkoutGameFeatureOutcome::Completed);
        QCOMPARE(result.route, WorkoutGameRoute::MainLine);
    }

    void weakSprintUsesSafeBypass()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(courseFor({{0, 10000, 250.0, 250.0}}), 200.0));

        simulation.update(sample(0, 100.0, 250.0, 50.0));
        const WorkoutGameSimulationSnapshot result =
                simulation.update(sample(7500, 100.0, 250.0, 50.0));

        QCOMPARE(result.featureOutcome, WorkoutGameFeatureOutcome::Bypassed);
        QCOMPARE(result.route, WorkoutGameRoute::SafeBypass);
    }

    void weakRollerEffortMissesFlowButStaysOnTheTrail()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(
                challengeCourse(WorkoutGameTerrainKind::Rollers), 200.0));

        WorkoutGameSimulationSnapshot result;
        for (std::int64_t time = 0; time <= 9500; time += 250) {
            result = simulation.update(sample(time, 80.0, 200.0, 45.0));
        }

        QCOMPARE(result.featureOutcome, WorkoutGameFeatureOutcome::Bypassed);
        QCOMPARE(result.route, WorkoutGameRoute::MainLine);
    }

    void tabletopUsesTargetPowerRegardlessOfSyntheticSpeed()
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
                 WorkoutGameFeatureOutcome::Completed);
        QCOMPARE(slowResult.challengeReadiness, 1.0);
        QCOMPARE(strongResult.score, slowResult.score);
    }

    void activeChallengeReportsReadinessBeforeDecision()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(
                challengeCourse(WorkoutGameTerrainKind::RockGarden), 200.0));
        WorkoutGameSimulationInput input = sample(0, 160.0, 200.0, 60.0);
        input.authoritativeSpeedKph = 6.0;
        const WorkoutGameSimulationSnapshot initial = simulation.update(input);
        const std::int64_t measurementStartMs = std::int64_t(std::ceil(
                initial.challenge.measurementStartProgress * 10000.0));
        const std::int64_t decisionMs = std::int64_t(std::floor(
                initial.challenge.decisionProgress * 10000.0));
        input.workoutTimeMs = measurementStartMs - 100;
        QCOMPARE(simulation.update(input).challengeReadiness, 0.0);
        input.workoutTimeMs = std::min(
                measurementStartMs + 500, decisionMs - 100);

        const WorkoutGameSimulationSnapshot result = simulation.update(input);

        QCOMPARE(result.featureOutcome, WorkoutGameFeatureOutcome::Active);
        QCOMPARE(result.challenge.cue, WorkoutGameChallengeCue::CarrySpeed);
        QVERIFY(result.challengeReadiness > 0.0);
        QVERIFY(result.challengeReadiness < 1.0);
    }

    void activeChallengeExposesMeasuredInputsAndPowerReadiness()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(
                challengeCourse(WorkoutGameTerrainKind::Tabletop), 200.0));

        WorkoutGameSimulationSnapshot result;
        for (std::int64_t time = 0; time <= 6800; time += 100) {
            WorkoutGameSimulationInput input = sample(
                    time, 190.0, 200.0, 35.0);
            input.authoritativeSpeedKph = 18.0;
            result = simulation.update(input);
        }

        QVERIFY(result.challengeMeasurementActive);
        QVERIFY(std::abs(result.challengeMetrics.averageActualWatts
                         - 190.0) < 1e-9);
        QVERIFY(std::abs(result.challengeMetrics.averageTargetWatts
                         - 200.0) < 1e-9);
        QVERIFY(std::abs(result.challengeMetrics.averageCadenceRpm
                         - 35.0) < 1e-9);
        QVERIFY(std::abs(result.challengeMetrics.averageSpeedKph
                         - 18.0) < 1e-9);
        QVERIFY(std::abs(result.challengeAssessment.effortReadiness
                         - 0.95) < 1e-9);
        QCOMPARE(result.challengeAssessment.cadenceReadiness, 1.0);
        QCOMPARE(result.challengeAssessment.speedReadiness, 1.0);
        QVERIFY(std::abs(result.challengeReadiness - 0.95) < 1e-9);
    }

    void jumpChallengeMeasuresTheTimedApproachWindow()
    {
        const WorkoutGameCourse course = challengeCourse(
                WorkoutGameTerrainKind::LogOver);
        WorkoutGameSimulation earlyOnly;
        WorkoutGameSimulation timedBurst;
        QVERIFY(earlyOnly.configure(course, 200.0));
        QVERIFY(timedBurst.configure(course, 200.0));

        WorkoutGameSimulationSnapshot earlyResult;
        WorkoutGameSimulationSnapshot timedResult;
        for (std::int64_t time = 0; time <= 7500; time += 250) {
            const bool inApproach = time >= 6400;
            WorkoutGameSimulationInput earlyInput = sample(
                    time, inApproach ? 100.0 : 200.0, 200.0,
                    inApproach ? 45.0 : 85.0);
            earlyInput.authoritativeSpeedKph = inApproach ? 6.0 : 20.0;
            earlyResult = earlyOnly.update(earlyInput);

            WorkoutGameSimulationInput burstInput = sample(
                    time, inApproach ? 200.0 : 100.0, 200.0,
                    inApproach ? 85.0 : 45.0);
            burstInput.authoritativeSpeedKph = inApproach ? 20.0 : 6.0;
            timedResult = timedBurst.update(burstInput);
        }

        QCOMPARE(earlyResult.featureOutcome,
                 WorkoutGameFeatureOutcome::Bypassed);
        QCOMPARE(timedResult.featureOutcome,
                 WorkoutGameFeatureOutcome::Completed);
    }

    void lateJumpBurstIsNotRejectedAsPoorAdherence()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(
                challengeCourse(WorkoutGameTerrainKind::LogOver), 200.0));

        WorkoutGameSimulationSnapshot result;
        for (std::int64_t time = 0; time <= 7500; time += 100) {
            const bool push = time >= 6800;
            WorkoutGameSimulationInput input = sample(
                    time, push ? 300.0 : 100.0, 200.0,
                    push ? 90.0 : 50.0);
            input.authoritativeSpeedKph = push ? 20.0 : 7.0;
            result = simulation.update(input);
        }
        QCOMPARE(result.featureOutcome,
                 WorkoutGameFeatureOutcome::Completed);
        QCOMPARE(result.route, WorkoutGameRoute::MainLine);
    }

    void longJumpSectionStillMeasuresOnlySixMetres()
    {
        WorkoutGameCourse course = challengeCourse(
                WorkoutGameTerrainKind::LogOver);
        course.durationMs = 120000;
        course.sections.front().durationMs = course.durationMs;
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        QVERIFY(road.ready);

        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(course, 200.0));
        const WorkoutGameSimulationSnapshot initial = simulation.update(
                sample(0, 200.0, 200.0));
        QVERIFY(initial.challenge.enabled);
        const double sectionLength = road.timeline.front().endDistanceMeters
                - road.timeline.front().startDistanceMeters;
        QVERIFY((initial.challenge.decisionProgress
                 - initial.challenge.measurementStartProgress)
                * sectionLength <= 6.0 + 1e-9);
    }

    void longClimbStillMeasuresTheWholeEffort()
    {
        WorkoutGameCourse course = challengeCourse(
                WorkoutGameTerrainKind::Climb);
        course.durationMs = 120000;
        course.sections.front().durationMs = course.durationMs;
        course.sections.front().gradePercent = 8.0;
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(course, 200.0));
        const WorkoutGameSimulationSnapshot initial = simulation.update(
                sample(0, 200.0, 200.0));
        QVERIFY(initial.challenge.enabled);
        QCOMPARE(initial.challenge.measurementStartProgress, 0.0);
        QCOMPARE(initial.challenge.decisionProgress, 0.95);
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
        stopped.workoutTimeMs = 1000;
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

    void virtualGearChangesSpeedThroughInertiaInsteadOfTeleporting()
    {
        WorkoutGameSimulation downshift;
        QVERIFY(downshift.configure(
                courseFor({{0, 60000, 180.0, 180.0}}), 200.0));
        WorkoutGameSimulationInput fast = sample(0, 180.0, 180.0);
        fast.authoritativeSpeedKph = 20.0;
        fast.drivetrainSpeedLimitKph = 35.0;
        fast.virtualGear = 10;
        QCOMPARE(downshift.update(fast).speedKph, 20.0);

        WorkoutGameSimulationInput lowGear = fast;
        lowGear.drivetrainSpeedLimitKph = 7.2;
        lowGear.virtualGear = 1;
        QCOMPARE(downshift.update(lowGear).speedKph, 20.0);
        lowGear.workoutTimeMs = 100;
        const double afterDownshift = downshift.update(lowGear).speedKph;
        QVERIFY(afterDownshift < 20.0);
        QVERIFY2(afterDownshift > 15.0,
                 "a downshift teleported the bicycle to the new cadence speed");

        WorkoutGameSimulation upshift;
        QVERIFY(upshift.configure(
                courseFor({{0, 60000, 180.0, 180.0}}), 200.0));
        WorkoutGameSimulationInput slow = sample(0, 180.0, 180.0);
        slow.authoritativeSpeedKph = 30.0;
        slow.drivetrainSpeedLimitKph = 7.2;
        slow.virtualGear = 1;
        QCOMPARE(upshift.update(slow).speedKph, 7.2);

        WorkoutGameSimulationInput highGear = slow;
        highGear.drivetrainSpeedLimitKph = 35.0;
        highGear.virtualGear = 10;
        QCOMPARE(upshift.update(highGear).speedKph, 7.2);
        highGear.workoutTimeMs = 100;
        const double afterUpshift = upshift.update(highGear).speedKph;
        QVERIFY(afterUpshift > 7.2);
        QVERIFY2(afterUpshift < 12.0,
                 "an upshift teleported the bicycle to the new cadence speed");
    }

    void everyVirtualGearHasContinuousFlatClimbAndDescentTraces()
    {
        constexpr double MaximumStepKph = 7.2 * 0.05 + 1e-9;
        double previousFlatSpeedKph = 0.0;
        double previousClimbSpeedKph = 0.0;

        for (int gear = 1; gear <= 12; ++gear) {
            const GearTrace flat = rideTrace(gear, 0.0, false);
            const GearTrace climb = rideTrace(gear, 8.0, false);
            const GearTrace descent = rideTrace(gear, -8.0, true);

            QVERIFY2(flat.maximumStepKph <= MaximumStepKph,
                     qPrintable(QStringLiteral("flat gear %1 stepped %2 kph")
                             .arg(gear).arg(flat.maximumStepKph)));
            QVERIFY2(climb.maximumStepKph <= MaximumStepKph,
                     qPrintable(QStringLiteral("climb gear %1 stepped %2 kph")
                             .arg(gear).arg(climb.maximumStepKph)));
            QVERIFY2(descent.maximumStepKph <= MaximumStepKph,
                     qPrintable(QStringLiteral("descent gear %1 stepped %2 kph")
                             .arg(gear).arg(descent.maximumStepKph)));
            QVERIFY(climb.finalSpeedKph <= flat.finalSpeedKph + 1e-9);
            QVERIFY2(descent.finalSpeedKph > flat.finalSpeedKph,
                     qPrintable(QStringLiteral(
                             "gear %1 descent %2 kph did not exceed flat %3 kph")
                             .arg(gear)
                             .arg(descent.finalSpeedKph)
                             .arg(flat.finalSpeedKph)));
            QVERIFY(flat.finalSpeedKph + 1e-9 >= previousFlatSpeedKph);
            QVERIFY(climb.finalSpeedKph + 1e-9 >= previousClimbSpeedKph);

            previousFlatSpeedKph = flat.finalSpeedKph;
            previousClimbSpeedKph = climb.finalSpeedKph;
        }

        const GearTrace lowFlat = rideTrace(1, 0.0, false);
        const GearTrace lowClimb = rideTrace(1, 8.0, false);
        QVERIFY(lowFlat.finalSpeedKph < 8.0);
        QVERIFY(lowClimb.finalSpeedKph < 8.0);
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

    void nonTrainerPowerEstimateStillRespectsVirtualGear()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(rideCourse(0.0), 200.0));

        WorkoutGameSimulationInput input = sample(0, 200.0, 200.0, 85.0);
        input.authoritativeSpeedKph = -1.0;
        input.drivetrainSpeedLimitKph = 7.2;
        input.virtualGear = 1;

        QCOMPARE(simulation.update(input).speedKph, 7.2);
    }

    void nonTrainerDescentCanFreewheelPastVirtualGear()
    {
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(rideCourse(-8.0, true), 200.0));

        WorkoutGameSimulationInput input = sample(0, 0.0, 100.0, 0.0);
        input.authoritativeSpeedKph = -1.0;
        input.drivetrainSpeedLimitKph = 7.2;
        input.virtualGear = 1;

        QVERIFY(simulation.update(input).speedKph > 30.0);
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
