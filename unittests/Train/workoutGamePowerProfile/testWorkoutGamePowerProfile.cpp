/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameFeatureLab.h"
#include "Train/WorkoutGamePowerProfile.h"

#include <QTest>

#include <cmath>

class TestWorkoutGamePowerProfile : public QObject
{
    Q_OBJECT

private slots:
    void invalidCourseFailsClosed()
    {
        QVERIFY(!WorkoutGamePowerProfile::build(
                WorkoutGameCourse(), WorkoutGameSimulationSnapshot(), 200.0).ready);
    }

    void exposesNormalizedPowerSegmentsAndCurrentCursor()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameSimulationSnapshot simulation;
        simulation.ready = true;
        simulation.activeSection = 2;
        simulation.workoutTimeMs = course.sections[2].startMs + 6000;
        simulation.sectionProgress = 0.5;
        const WorkoutGamePowerProfileSnapshot profile =
                WorkoutGamePowerProfile::build(course, simulation, 215.0);

        QVERIFY(profile.ready);
        QCOMPARE(profile.segments.size(), course.sections.size());
        QVERIFY(std::abs(profile.cursor - double(simulation.workoutTimeMs)
                / double(course.durationMs)) < 1e-9);
        QCOMPARE(profile.actualWatts, 215.0);
        QVERIFY(profile.maximumWatts >= course.sections[6].targetWatts);
        QCOMPARE(profile.segments.front().start, 0.0);
        QCOMPARE(profile.segments.back().end, 1.0);
    }

    void tellsTheRiderWhenToPushAndHowMuch()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameSimulationSnapshot simulation;
        simulation.ready = true;
        simulation.activeSection = 0;
        simulation.challenge = WorkoutGameFeatureChallenge::profile(
                course.sections.front());
        simulation.featureOutcome = WorkoutGameFeatureOutcome::Active;

        simulation.sectionProgress = 0.30;
        simulation.workoutTimeMs = 3600;
        const WorkoutGamePowerProfileSnapshot prepare =
                WorkoutGamePowerProfile::build(course, simulation, 180.0);
        QCOMPARE(prepare.cue.state, WorkoutGamePowerCueState::Prepare);
        QVERIFY(prepare.cue.secondsUntilWindow > 1.0);

        simulation.sectionProgress = 0.68;
        simulation.workoutTimeMs = 8160;
        simulation.speedKph = 18.0;
        const WorkoutGamePowerProfileSnapshot push =
                WorkoutGamePowerProfile::build(
                    course, simulation, 210.0, 80.0);
        QCOMPARE(push.cue.state, WorkoutGamePowerCueState::PushNow);
        QCOMPARE(push.cue.challengeCue, WorkoutGameChallengeCue::Jump);
        QCOMPARE(push.cue.terrain, WorkoutGameTerrainKind::LogOver);
        QCOMPARE(push.cue.requiredWatts,
                 course.sections.front().targetWatts);
        QCOMPARE(push.cue.actualWatts, 210.0);
        QCOMPARE(push.cue.actualCadenceRpm, 80.0);
        QCOMPARE(push.cue.actualSpeedKph, 18.0);
        QVERIFY(push.cue.powerRequired);
        QVERIFY(!push.cue.cadenceRequired);
        QVERIFY(!push.cue.speedRequired);
        QVERIFY(push.cue.windowProgress > 0.0);
        QVERIFY(push.cue.windowProgress < 1.0);

        simulation.sectionProgress = 0.76;
        simulation.workoutTimeMs = 9120;
        simulation.featureOutcome = WorkoutGameFeatureOutcome::Completed;
        simulation.challengeMeasurementActive = true;
        simulation.challengeMetrics.averageActualWatts = 220.0;
        simulation.challengeMetrics.averageTargetWatts = 200.0;
        simulation.challengeMetrics.averageEffortRatio = 1.1;
        simulation.challengeMetrics.averageCadenceRpm = 85.0;
        simulation.challengeMetrics.averageSpeedKph = 20.0;
        simulation.challengeMetrics.averageAdherence = 1.0;
        simulation.challengeReadiness = 1.0;
        const WorkoutGamePowerProfileSnapshot committed =
                WorkoutGamePowerProfile::build(course, simulation, 220.0);
        QCOMPARE(committed.cue.state, WorkoutGamePowerCueState::Committed);
        QCOMPARE(committed.cue.readiness, 1.0);
        QCOMPARE(committed.cue.actualWatts, 220.0);
        QVERIFY(std::abs(committed.cue.requiredWatts
                         - 200.0 * simulation.challenge.minimumEffortRatio)
                < 1e-9);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGamePowerProfile)
#include "testWorkoutGamePowerProfile.moc"
