/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameFeatureHud.h"

#include <QTest>

#include <limits>

namespace {

WorkoutGameFeatureRuntimeSnapshot feature(
        WorkoutGameFeaturePhase phase,
        WorkoutGameFeatureOutcome outcome = WorkoutGameFeatureOutcome::Active)
{
    WorkoutGameFeatureRuntimeSnapshot result;
    result.ready = true;
    result.terrain = WorkoutGameTerrainKind::Tabletop;
    result.phase = phase;
    result.outcome = outcome;
    result.visualDistanceMeters = 12.0;
    result.prepareDistanceMeters = 10.0;
    result.decisionDistanceMeters = 20.0;
    result.actionStartDistanceMeters = 24.0;
    result.actionEndDistanceMeters = 28.0;
    result.obstacleDistanceMeters = 25.0;
    return result;
}

WorkoutGameSimulationSnapshot simulation(
        double effortReadiness,
        double cadenceReadiness,
        double minimumEffortRatio = 1.0,
        double minimumCadenceRpm = 0.0)
{
    WorkoutGameSimulationSnapshot result;
    result.ready = true;
    result.challenge.enabled = true;
    result.challenge.minimumEffortRatio = minimumEffortRatio;
    result.challenge.minimumCadenceRpm = minimumCadenceRpm;
    result.challengeAssessment.effortReadiness = effortReadiness;
    result.challengeAssessment.cadenceReadiness = cadenceReadiness;
    return result;
}

}

class TestWorkoutGameFeatureHud : public QObject
{
    Q_OBJECT

private slots:
    void hidesUnavailableFeature()
    {
        const WorkoutGameFeatureHudSnapshot result =
                WorkoutGameFeatureHud::build({}, {}, 200.0);

        QVERIFY(!result.visible);
        QCOMPARE(result.state, WorkoutGameFeatureHudState::Hidden);
    }

    void prepareShowsDecisionDistanceAndPowerRequirement()
    {
        const WorkoutGameFeatureHudSnapshot result = WorkoutGameFeatureHud::build(
                feature(WorkoutGameFeaturePhase::Approach),
                simulation(0.4, 1.0),
                200.0);

        QVERIFY(result.visible);
        QCOMPARE(result.state, WorkoutGameFeatureHudState::Prepare);
        QCOMPARE(result.distanceKind,
                 WorkoutGameFeatureHudDistanceKind::Decision);
        QCOMPARE(result.distanceMeters, 8.0);
        QVERIFY(result.powerRequired);
        QCOMPARE(result.requiredPowerWatts, 200.0);
        QCOMPARE(result.powerReadinessPercent, 40);
        QVERIFY(!result.cadenceRequired);
        QCOMPARE(result.cadenceReadinessPercent, 100);
    }

    void measurementKeepsPowerAndCadenceSeparate()
    {
        WorkoutGameFeatureRuntimeSnapshot runtime = feature(
                WorkoutGameFeaturePhase::Measure);
        runtime.visualDistanceMeters = 16.0;
        const WorkoutGameFeatureHudSnapshot result = WorkoutGameFeatureHud::build(
                runtime,
                simulation(0.65, 0.8, 1.1, 75.0),
                240.0);

        QCOMPARE(result.state, WorkoutGameFeatureHudState::Measure);
        QCOMPARE(result.distanceMeters, 4.0);
        QCOMPARE(result.requiredPowerWatts, 264.0);
        QCOMPARE(result.powerReadinessPercent, 65);
        QVERIFY(result.cadenceRequired);
        QCOMPARE(result.requiredCadenceRpm, 75.0);
        QCOMPARE(result.cadenceReadinessPercent, 80);
    }

    void committedLineCountsDownToAction()
    {
        WorkoutGameFeatureRuntimeSnapshot runtime = feature(
                WorkoutGameFeaturePhase::Committed,
                WorkoutGameFeatureOutcome::Completed);
        runtime.visualDistanceMeters = 21.5;
        runtime.route = WorkoutGameRoute::MainLine;
        const WorkoutGameFeatureHudSnapshot result = WorkoutGameFeatureHud::build(
                runtime, simulation(1.0, 1.0), 200.0);

        QCOMPARE(result.state, WorkoutGameFeatureHudState::Committed);
        QCOMPARE(result.route, WorkoutGameRoute::MainLine);
        QCOMPARE(result.distanceKind,
                 WorkoutGameFeatureHudDistanceKind::Action);
        QCOMPARE(result.distanceMeters, 2.5);
    }

    void actionHasNoEarlyDistanceCue()
    {
        const WorkoutGameFeatureHudSnapshot result = WorkoutGameFeatureHud::build(
                feature(WorkoutGameFeaturePhase::Action,
                        WorkoutGameFeatureOutcome::Completed),
                simulation(1.0, 1.0),
                200.0);

        QCOMPARE(result.state, WorkoutGameFeatureHudState::ActNow);
        QCOMPARE(result.distanceKind,
                 WorkoutGameFeatureHudDistanceKind::None);
        QCOMPARE(result.distanceMeters, 0.0);
    }

    void missedClimbStaysAnActiveMainLineEffort()
    {
        WorkoutGameFeatureRuntimeSnapshot runtime = feature(
                WorkoutGameFeaturePhase::Action,
                WorkoutGameFeatureOutcome::Bypassed);
        runtime.terrain = WorkoutGameTerrainKind::Climb;
        runtime.route = WorkoutGameRoute::MainLine;
        const WorkoutGameFeatureHudSnapshot result = WorkoutGameFeatureHud::build(
                runtime, simulation(0.45, 1.0), 220.0);

        QVERIFY(result.visible);
        QCOMPARE(result.route, WorkoutGameRoute::MainLine);
        QCOMPARE(result.state, WorkoutGameFeatureHudState::ActNow);

        runtime.phase = WorkoutGameFeaturePhase::Recovery;
        runtime.visualDistanceMeters = 29.0;
        const WorkoutGameFeatureHudSnapshot recovery =
                WorkoutGameFeatureHud::build(
                    runtime, simulation(0.45, 1.0), 220.0);
        QVERIFY(recovery.visible);
        QCOMPARE(recovery.state, WorkoutGameFeatureHudState::NoBonus);
    }

    void recoveryReportsCompletedAndBypassedOutcomes_data()
    {
        QTest::addColumn<int>("outcome");
        QTest::addColumn<int>("state");
        QTest::newRow("complete")
                << int(WorkoutGameFeatureOutcome::Completed)
                << int(WorkoutGameFeatureHudState::Complete);
        QTest::newRow("bypass")
                << int(WorkoutGameFeatureOutcome::Bypassed)
                << int(WorkoutGameFeatureHudState::Bypass);
    }

    void recoveryReportsCompletedAndBypassedOutcomes()
    {
        QFETCH(int, outcome);
        QFETCH(int, state);
        WorkoutGameFeatureRuntimeSnapshot runtime = feature(
                WorkoutGameFeaturePhase::Recovery,
                WorkoutGameFeatureOutcome(outcome));
        runtime.visualDistanceMeters = 29.0;
        const WorkoutGameFeatureHudSnapshot result = WorkoutGameFeatureHud::build(
                runtime,
                simulation(1.0, 1.0),
                200.0);

        QCOMPARE(int(result.state), state);
        QCOMPARE(result.distanceKind,
                 WorkoutGameFeatureHudDistanceKind::None);
    }

    void approachCueStartsNearTheMeasurementZone()
    {
        WorkoutGameFeatureRuntimeSnapshot runtime = feature(
                WorkoutGameFeaturePhase::Approach);
        runtime.visualDistanceMeters = 0.0;
        runtime.prepareDistanceMeters = 20.0;
        runtime.decisionDistanceMeters = 28.0;
        QVERIFY(!WorkoutGameFeatureHud::build(
                runtime, simulation(0.0, 1.0), 200.0).visible);

        runtime.visualDistanceMeters = 8.0;
        const WorkoutGameFeatureHudSnapshot result = WorkoutGameFeatureHud::build(
                runtime, simulation(0.0, 1.0), 200.0);
        QVERIFY(result.visible);
        QCOMPARE(result.state, WorkoutGameFeatureHudState::Prepare);
        QCOMPARE(result.distanceMeters, 20.0);
    }

    void resultCueEndsAfterSixMetres()
    {
        WorkoutGameFeatureRuntimeSnapshot runtime = feature(
                WorkoutGameFeaturePhase::Recovery,
                WorkoutGameFeatureOutcome::Completed);
        runtime.visualDistanceMeters = 34.0;
        QVERIFY(WorkoutGameFeatureHud::build(
                runtime, simulation(1.0, 1.0), 200.0).visible);

        runtime.visualDistanceMeters = 34.01;
        QVERIFY(!WorkoutGameFeatureHud::build(
                runtime, simulation(1.0, 1.0), 200.0).visible);
    }

    void malformedNumbersAreBounded()
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        WorkoutGameFeatureRuntimeSnapshot runtime = feature(
                WorkoutGameFeaturePhase::Measure);
        runtime.visualDistanceMeters = nan;
        runtime.decisionDistanceMeters = nan;
        WorkoutGameSimulationSnapshot state = simulation(nan, nan, nan, nan);

        const WorkoutGameFeatureHudSnapshot result =
                WorkoutGameFeatureHud::build(runtime, state, nan);

        QCOMPARE(result.distanceMeters, 0.0);
        QCOMPARE(result.requiredPowerWatts, 0.0);
        QCOMPARE(result.requiredCadenceRpm, 0.0);
        QCOMPARE(result.powerReadinessPercent, 0);
        QCOMPARE(result.cadenceReadinessPercent, 0);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameFeatureHud)
#include "testWorkoutGameFeatureHud.moc"
