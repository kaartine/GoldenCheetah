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

        simulation.sectionProgress = 0.55;
        simulation.workoutTimeMs = 6600;
        const WorkoutGamePowerProfileSnapshot push =
                WorkoutGamePowerProfile::build(course, simulation, 210.0);
        QCOMPARE(push.cue.state, WorkoutGamePowerCueState::PushNow);
        QVERIFY(push.cue.requiredWatts > 190.0);
        QVERIFY(push.cue.windowProgress > 0.0);
        QVERIFY(push.cue.windowProgress < 1.0);

        simulation.sectionProgress = 0.72;
        simulation.workoutTimeMs = 8640;
        simulation.featureOutcome = WorkoutGameFeatureOutcome::Completed;
        simulation.challengeReadiness = 1.0;
        const WorkoutGamePowerProfileSnapshot committed =
                WorkoutGamePowerProfile::build(course, simulation, 220.0);
        QCOMPARE(committed.cue.state, WorkoutGamePowerCueState::Committed);
        QCOMPARE(committed.cue.readiness, 1.0);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGamePowerProfile)
#include "testWorkoutGamePowerProfile.moc"
