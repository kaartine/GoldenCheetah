/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameFeatureLab.h"
#include "Train/WorkoutGameFeatureRuntime.h"

#include <QTest>

#include <cmath>

namespace {

WorkoutGameSimulationSnapshot snapshot(
        int section,
        double progress,
        WorkoutGameFeatureOutcome outcome,
        WorkoutGameRoute route = WorkoutGameRoute::MainLine)
{
    WorkoutGameSimulationSnapshot result;
    result.ready = true;
    result.activeSection = section;
    result.sectionProgress = progress;
    result.featureOutcome = outcome;
    result.route = route;
    result.challengeReadiness = outcome == WorkoutGameFeatureOutcome::Completed
            ? 1.0 : 0.45;
    return result;
}

int sectionFor(
        const WorkoutGameCourse &course,
        WorkoutGameTerrainKind terrain)
{
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        if (course.sections[index].terrain == terrain
                && course.sections[index].challengeCount > 0) {
            return int(index);
        }
    }
    return -1;
}

}

class TestWorkoutGameFeatureRuntime : public QObject
{
    Q_OBJECT

private slots:
    void invalidCoursesFailClosed()
    {
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(!runtime.configure(WorkoutGameRoadCourse()));
        QVERIFY(!runtime.update(WorkoutGameSimulationSnapshot()).ready);
    }

    void phaseProgressesThroughTheAnchoredGate()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(
                WorkoutGameRoadCourseBuilder::build(course, 200.0)));
        const int section = sectionFor(course, WorkoutGameTerrainKind::LogOver);

        QCOMPARE(runtime.update(snapshot(
                    section, 0.10, WorkoutGameFeatureOutcome::Active)).phase,
                 WorkoutGameFeaturePhase::Approach);
        QCOMPARE(runtime.update(snapshot(
                    section, 0.68, WorkoutGameFeatureOutcome::Active)).phase,
                 WorkoutGameFeaturePhase::Measure);
        QCOMPARE(runtime.update(snapshot(
                    section, 0.74, WorkoutGameFeatureOutcome::Completed)).phase,
                 WorkoutGameFeaturePhase::Committed);
        QCOMPARE(runtime.update(snapshot(
                    section, 0.94, WorkoutGameFeatureOutcome::Completed)).phase,
                 WorkoutGameFeaturePhase::Action);
        QCOMPARE(runtime.update(snapshot(
                    section, 0.995, WorkoutGameFeatureOutcome::Completed)).phase,
                 WorkoutGameFeaturePhase::Recovery);
    }

    void completedLogJumpsAtTheObstacle()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(
                WorkoutGameRoadCourseBuilder::build(course, 200.0)));
        const int section = sectionFor(course, WorkoutGameTerrainKind::LogOver);

        const WorkoutGameFeatureRuntimeSnapshot result = runtime.update(
                snapshot(section, 0.94,
                         WorkoutGameFeatureOutcome::Completed));

        QCOMPARE(result.motion, WorkoutGameFeatureMotion::Jump);
        QVERIFY(result.triggerJump);
        QVERIFY(result.verticalOffsetMeters > 0.2);
        QCOMPARE(result.lateralOffset, 0.0);
        QVERIFY(result.actionId != 0u);
    }

    void missedLogUsesVisibleBypassWithoutJumping()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(
                WorkoutGameRoadCourseBuilder::build(course, 200.0)));
        const int section = sectionFor(course, WorkoutGameTerrainKind::LogOver);

        const WorkoutGameFeatureRuntimeSnapshot result = runtime.update(
                snapshot(section, 0.94,
                         WorkoutGameFeatureOutcome::Bypassed,
                         WorkoutGameRoute::SafeBypass));

        QVERIFY(!result.triggerJump);
        QCOMPARE(result.verticalOffsetMeters, 0.0);
        QVERIFY(std::abs(result.lateralOffset) > 0.1);

        const WorkoutGameFeatureRuntimeSnapshot recovered = runtime.update(
                snapshot(section, 0.995,
                         WorkoutGameFeatureOutcome::Bypassed,
                         WorkoutGameRoute::SafeBypass));
        QVERIFY(std::abs(recovered.lateralOffset) < 1e-9);
    }

    void rootsAndRockGardenExposeDifferentRoughness()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(
                WorkoutGameRoadCourseBuilder::build(course, 200.0)));
        const auto completed = WorkoutGameFeatureOutcome::Completed;
        const WorkoutGameFeatureRuntimeSnapshot roots = runtime.update(snapshot(
                sectionFor(course, WorkoutGameTerrainKind::Roots),
                0.94, completed));
        const WorkoutGameFeatureRuntimeSnapshot rocks = runtime.update(snapshot(
                sectionFor(course, WorkoutGameTerrainKind::RockGarden),
                0.94, completed));

        QCOMPARE(roots.motion, WorkoutGameFeatureMotion::Absorb);
        QCOMPARE(rocks.motion, WorkoutGameFeatureMotion::Absorb);
        QVERIFY(roots.vibration > 0.0);
        QVERIFY(rocks.vibration > roots.vibration);
    }

    void tabletopHasMoreAirThanTheLog()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(
                WorkoutGameRoadCourseBuilder::build(course, 200.0)));
        const auto completed = WorkoutGameFeatureOutcome::Completed;
        const WorkoutGameFeatureRuntimeSnapshot log = runtime.update(snapshot(
                sectionFor(course, WorkoutGameTerrainKind::LogOver),
                0.94, completed));
        const WorkoutGameFeatureRuntimeSnapshot tabletop = runtime.update(snapshot(
                sectionFor(course, WorkoutGameTerrainKind::Tabletop),
                0.94, completed));

        QCOMPARE(tabletop.motion, WorkoutGameFeatureMotion::Jump);
        QVERIFY(tabletop.verticalOffsetMeters > log.verticalOffsetMeters);
    }

    void jumpArcHasVisibleTakeoffApexAndLanding()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(
                WorkoutGameRoadCourseBuilder::build(course, 200.0)));
        const int section = sectionFor(course, WorkoutGameTerrainKind::Tabletop);
        const auto completed = WorkoutGameFeatureOutcome::Completed;

        const WorkoutGameFeatureRuntimeSnapshot takeoff = runtime.update(
                snapshot(section, 0.79, completed));
        const WorkoutGameFeatureRuntimeSnapshot apex = runtime.update(
                snapshot(section, 0.84, completed));
        const WorkoutGameFeatureRuntimeSnapshot landing = runtime.update(
                snapshot(section, 0.89, completed));

        QCOMPARE(takeoff.phase, WorkoutGameFeaturePhase::Action);
        QCOMPARE(apex.phase, WorkoutGameFeaturePhase::Action);
        QCOMPARE(landing.phase, WorkoutGameFeaturePhase::Action);
        QVERIFY(takeoff.verticalOffsetMeters > 0.05);
        QVERIFY(apex.verticalOffsetMeters > takeoff.verticalOffsetMeters);
        QVERIFY(apex.verticalOffsetMeters > landing.verticalOffsetMeters);
        QVERIFY(landing.verticalOffsetMeters > 0.05);
    }

    void bypassLineEasesInAndOutWithoutLateralJumps()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(
                WorkoutGameRoadCourseBuilder::build(course, 200.0)));
        const int section = sectionFor(course, WorkoutGameTerrainKind::LogOver);
        const auto bypassed = WorkoutGameFeatureOutcome::Bypassed;
        const auto route = WorkoutGameRoute::SafeBypass;

        const double atBranch = runtime.update(
                snapshot(section, 0.65, bypassed, route)).lateralOffset;
        const double justAfterBranch = runtime.update(
                snapshot(section, 0.655, bypassed, route)).lateralOffset;
        const double nearReturn = runtime.update(
                snapshot(section, 0.955, bypassed, route)).lateralOffset;
        const double afterReturn = runtime.update(
                snapshot(section, 0.96, bypassed, route)).lateralOffset;

        QVERIFY(std::abs(atBranch) < 1e-9);
        QVERIFY(std::abs(justAfterBranch - atBranch) < 0.01);
        QVERIFY(std::abs(afterReturn - nearReturn) < 0.01);
        QVERIFY(std::abs(afterReturn) < 1e-9);
    }

    void dropPitchesTheRiderAndReportsLanding()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(
                WorkoutGameRoadCourseBuilder::build(course, 200.0)));
        const int section = sectionFor(course, WorkoutGameTerrainKind::Drop);

        const WorkoutGameFeatureRuntimeSnapshot action = runtime.update(snapshot(
                section, 0.94, WorkoutGameFeatureOutcome::Completed));
        const WorkoutGameFeatureRuntimeSnapshot recovery = runtime.update(snapshot(
                section, 0.995, WorkoutGameFeatureOutcome::Completed));

        QCOMPARE(action.motion, WorkoutGameFeatureMotion::Drop);
        QVERIFY(action.pitchDegrees < -2.0);
        QVERIFY(recovery.landingImpact > 0.0);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameFeatureRuntime)
#include "testWorkoutGameFeatureRuntime.moc"
