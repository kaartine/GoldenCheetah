/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameFeatureLab.h"
#include "Train/WorkoutGameRunner.h"

#include <QElapsedTimer>
#include <QTest>

#include <cmath>
#include <optional>

namespace {

bool waitForFrame(
        WorkoutGameRunner &runner,
        WorkoutGameEngineFrame &frame,
        int timeoutMs = 1000)
{
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < timeoutMs) {
        if (runner.takeLatest(frame)) return true;
        QTest::qWait(2);
    }
    return false;
}

int sectionForTerrain(
        const WorkoutGameCourse &course,
        WorkoutGameTerrainKind terrain)
{
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        if (course.sections[index].terrain == terrain) return int(index);
    }
    return -1;
}

bool waitForFrameAtOrAfter(
        WorkoutGameRunner &runner,
        std::int64_t workoutTimeMs,
        WorkoutGameEngineFrame &frame,
        int timeoutMs = 1000)
{
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < timeoutMs) {
        if (runner.takeLatest(frame)
                && frame.visual.simulation.workoutTimeMs >= workoutTimeMs) {
            return true;
        }
        QTest::qWait(2);
    }
    return false;
}

}

class TestWorkoutGameRunner : public QObject
{
    Q_OBJECT

private slots:
    void defaultConfigureDoesNotSynthesizeMissingGapTelemetry()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const int gapSection = sectionForTerrain(
                course, WorkoutGameTerrainKind::GapJump);
        QVERIFY(gapSection >= 0);

        WorkoutGameRunner runner;
        QVERIFY(runner.configure(course, 200.0, true));
        runner.start(course.sections[std::size_t(gapSection)].startMs + 200,
                     1.0);

        WorkoutGameEngineFrame frame;
        QVERIFY(waitForFrame(runner, frame));
        QVERIFY(frame.telemetryStale);
        QCOMPARE(frame.visual.simulation.activeSection, gapSection);
        QCOMPARE(frame.watts, 0.0);
        QCOMPARE(frame.cadenceRpm, 0);
    }

    void disabledFeatureLabIgnoresProvidedGapScenario()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const int gapSection = sectionForTerrain(
                course, WorkoutGameTerrainKind::GapJump);
        QVERIFY(gapSection >= 0);

        WorkoutGameRunner runner;
        QVERIFY(runner.configure(
                course, 200.0, false,
                std::optional<WorkoutGameFeatureLabGapScenario>(
                    WorkoutGameFeatureLabGapScenario::Long)));
        runner.start(course.sections[std::size_t(gapSection)].startMs + 200,
                     1.0);

        WorkoutGameEngineFrame frame;
        QVERIFY(waitForFrame(runner, frame));
        QVERIFY(frame.telemetryStale);
        QCOMPARE(frame.visual.simulation.activeSection, gapSection);
        QCOMPARE(frame.watts, 0.0);
        QCOMPARE(frame.cadenceRpm, 0);
    }

    void gapScenarioRunsWithoutTelemetryAtRunnerTime()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const int gapSection = sectionForTerrain(
                course, WorkoutGameTerrainKind::GapJump);
        QVERIFY(gapSection >= 0);
        const std::int64_t anchor =
                course.sections[std::size_t(gapSection)].startMs + 200;
        WorkoutGameSimulationInput expected;
        QVERIFY(WorkoutGameFeatureLab::applyGapScenario(
                course, anchor, WorkoutGameFeatureLabGapScenario::Short,
                expected));

        WorkoutGameRunner runner;
        QVERIFY(runner.configure(
                course, 200.0, true,
                std::optional<WorkoutGameFeatureLabGapScenario>(
                    WorkoutGameFeatureLabGapScenario::Short)));
        runner.start(anchor, 1.0);

        WorkoutGameEngineFrame frame;
        QVERIFY(waitForFrame(runner, frame));
        QVERIFY(frame.telemetryStale);
        QCOMPARE(frame.visual.simulation.activeSection, gapSection);
        QCOMPARE(frame.watts, expected.actualWatts);
        QCOMPARE(frame.targetWatts, expected.targetWatts);
        QCOMPARE(frame.cadenceRpm, int(expected.cadenceRpm));
        QCOMPARE(frame.virtualGear, expected.virtualGear);
        QVERIFY(std::abs(frame.visual.simulation.speedKph
                         - expected.authoritativeSpeedKph) < 1.0e-9);
    }

    void gapScenarioOverridesTelemetryOnlyAfterStaleHandling()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const int gapSection = sectionForTerrain(
                course, WorkoutGameTerrainKind::GapJump);
        QVERIFY(gapSection >= 0);
        const std::int64_t anchor =
                course.sections[std::size_t(gapSection)].startMs + 400;
        WorkoutGameSimulationInput expected;
        QVERIFY(WorkoutGameFeatureLab::applyGapScenario(
                course, anchor, WorkoutGameFeatureLabGapScenario::Medium,
                expected));

        WorkoutGameRunner runner;
        QVERIFY(runner.configure(
                course, 200.0, true,
                std::optional<WorkoutGameFeatureLabGapScenario>(
                    WorkoutGameFeatureLabGapScenario::Medium)));
        WorkoutGameEngineInput stale;
        stale.simulation.workoutTimeMs = 0;
        stale.simulation.actualWatts = 999.0;
        stale.simulation.targetWatts = 7.0;
        stale.simulation.cadenceRpm = 199.0;
        stale.simulation.authoritativeSpeedKph = 99.0;
        stale.heartRate = 177;
        stale.telemetryMonotonicTimeMs =
                WorkoutGameRunner::monotonicMilliseconds() - 5000;
        runner.setTelemetry(stale);
        runner.start(anchor, 1.0);

        WorkoutGameEngineFrame frame;
        QVERIFY(waitForFrame(runner, frame));
        QVERIFY(frame.telemetryStale);
        QCOMPARE(frame.visual.simulation.activeSection, gapSection);
        QCOMPARE(frame.watts, expected.actualWatts);
        QCOMPARE(frame.targetWatts, expected.targetWatts);
        QCOMPARE(frame.cadenceRpm, int(expected.cadenceRpm));
        QCOMPARE(frame.heartRate, 0);
        QVERIFY(std::abs(frame.visual.simulation.speedKph
                         - expected.authoritativeSpeedKph) < 1.0e-9);
    }

    void gapScenarioUsesAuthoritativeSectionBoundaryNotTelemetryTime()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const int gapSection = sectionForTerrain(
                course, WorkoutGameTerrainKind::GapJump);
        QVERIFY(gapSection > 0);
        const std::int64_t gapStart =
                course.sections[std::size_t(gapSection)].startMs;

        WorkoutGameRunner runner;
        QVERIFY(runner.configure(
                course, 200.0, true,
                std::optional<WorkoutGameFeatureLabGapScenario>(
                    WorkoutGameFeatureLabGapScenario::Short)));
        WorkoutGameEngineInput telemetry;
        telemetry.simulation.workoutTimeMs = gapStart + 5000;
        telemetry.simulation.actualWatts = 17.0;
        telemetry.simulation.targetWatts = 19.0;
        telemetry.simulation.cadenceRpm = 43.0;
        telemetry.telemetryMonotonicTimeMs =
                WorkoutGameRunner::monotonicMilliseconds();
        runner.setTelemetry(telemetry);
        runner.start(gapStart - 160, 1.0);

        WorkoutGameEngineFrame before;
        QVERIFY(waitForFrame(runner, before));
        QVERIFY(before.visual.simulation.workoutTimeMs < gapStart);
        QCOMPARE(before.visual.simulation.activeSection, gapSection - 1);
        QCOMPARE(before.watts, 17.0);
        QCOMPARE(before.cadenceRpm, 43);

        WorkoutGameEngineFrame after;
        QVERIFY(waitForFrameAtOrAfter(runner, gapStart, after));
        QCOMPARE(after.visual.simulation.activeSection, gapSection);
        QCOMPARE(after.watts,
                 course.sections[std::size_t(gapSection)].targetWatts);
        QCOMPARE(after.cadenceRpm, 88);
    }

    void gapScenarioSurvivesPauseResumeResynchronization()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const int gapSection = sectionForTerrain(
                course, WorkoutGameTerrainKind::GapJump);
        QVERIFY(gapSection >= 0);
        const std::int64_t gapStart =
                course.sections[std::size_t(gapSection)].startMs;

        WorkoutGameRunner runner;
        QVERIFY(runner.configure(
                course, 200.0, true,
                std::optional<WorkoutGameFeatureLabGapScenario>(
                    WorkoutGameFeatureLabGapScenario::Long)));
        runner.start(gapStart + 200, 1.0);
        WorkoutGameEngineFrame first;
        QVERIFY(waitForFrame(runner, first));
        const std::uint64_t generation = first.visual.sessionGeneration;

        runner.pause(first.visual.simulation.workoutTimeMs);
        const std::int64_t resumedAt = gapStart + 2500;
        runner.resume(resumedAt, 1.0);
        WorkoutGameEngineFrame resumed;
        QVERIFY(waitForFrameAtOrAfter(runner, resumedAt, resumed));
        QVERIFY(resumed.visual.sessionGeneration > generation);
        QCOMPARE(resumed.visual.simulation.activeSection, gapSection);
        QCOMPARE(resumed.watts,
                 course.sections[std::size_t(gapSection)].targetWatts);
        QCOMPARE(resumed.cadenceRpm, 88);
        QVERIFY(resumed.telemetryStale);
    }

    void publishesLatestFixedStepFrameWithoutGuiDrivenSimulation()
    {
        WorkoutGameRunner runner;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        QVERIFY(runner.configure(course, 200.0, true));

        WorkoutGameEngineInput input;
        input.simulation.actualWatts = 220.0;
        input.simulation.targetWatts = 200.0;
        input.simulation.cadenceRpm = 88.0;
        input.simulation.virtualGear = 8;
        input.heartRate = 142;
        input.telemetryMonotonicTimeMs =
                WorkoutGameRunner::monotonicMilliseconds();
        runner.setTelemetry(input);
        runner.start(0, 1.0);

        QTest::qWait(130);
        WorkoutGameEngineFrame first;
        QVERIFY(waitForFrame(runner, first));
        QVERIFY(first.visual.simulation.workoutTimeMs >= 80);
        QVERIFY(first.visual.presentationTimeMs > 0);
        QVERIFY(first.visual.sessionGeneration > 0);
        QCOMPARE(first.heartRate, 142);

        QTest::qWait(80);
        WorkoutGameEngineFrame second;
        QVERIFY(waitForFrame(runner, second));
        QVERIFY(second.visual.simulation.workoutTimeMs
                > first.visual.simulation.workoutTimeMs);
        QVERIFY(second.visual.world.rider.distanceMeters
                >= first.visual.world.rider.distanceMeters);
    }

    void pauseStopsTicksAndResumeDoesNotResetProgress()
    {
        WorkoutGameRunner runner;
        QVERIFY(runner.configure(
                WorkoutGameFeatureLab::course(200.0), 200.0, true));
        WorkoutGameEngineFrame frame;
        runner.start(1000, 1.0);
        QTest::qWait(90);
        QVERIFY(waitForFrame(runner, frame));
        const std::int64_t beforePause = frame.visual.simulation.workoutTimeMs;
        const std::uint64_t beforeGeneration = frame.visual.sessionGeneration;
        while (runner.takeLatest(frame)) {}

        runner.pause(beforePause);
        QTest::qWait(80);
        QVERIFY(!runner.takeLatest(frame));

        runner.resume(beforePause, 1.0);
        QVERIFY(waitForFrame(runner, frame));
        QVERIFY(frame.visual.simulation.workoutTimeMs >= beforePause);
        QVERIFY(frame.visual.sessionGeneration > beforeGeneration);
    }

    void rapidPauseResumeNeverPublishesRetiredGeneration()
    {
        WorkoutGameRunner runner;
        QVERIFY(runner.configure(
                WorkoutGameFeatureLab::course(200.0), 200.0, true));
        runner.start(0, 1.0);
        WorkoutGameEngineFrame frame;
        QVERIFY(waitForFrame(runner, frame));
        std::uint64_t generation = frame.visual.sessionGeneration;

        for (int transition = 0; transition < 20; ++transition) {
            const std::int64_t anchor =
                    frame.visual.simulation.workoutTimeMs;
            runner.pause(anchor);
            while (runner.takeLatest(frame)) {}
            QTest::qWait(3);
            QVERIFY(!runner.takeLatest(frame));

            runner.resume(anchor, 1.0);
            QVERIFY(waitForFrame(runner, frame));
            QVERIFY(frame.visual.sessionGeneration > generation);
            QVERIFY(frame.visual.simulation.workoutTimeMs >= anchor);
            generation = frame.visual.sessionGeneration;
        }
    }

    void anchorChangeRetiresQueuedFrames()
    {
        WorkoutGameRunner runner;
        QVERIFY(runner.configure(
                WorkoutGameFeatureLab::course(200.0), 200.0, true));
        runner.start(0, 1.0);
        WorkoutGameEngineFrame frame;
        QVERIFY(waitForFrame(runner, frame));
        QTest::qWait(60);

        runner.setAnchor(5000, 1.0);
        QVERIFY(!runner.takeLatest(frame));
        QVERIFY(waitForFrame(runner, frame));
        QVERIFY(frame.visual.simulation.workoutTimeMs >= 5000);
    }

    void stopReturnsNewestUnconsumedFrameBeforeRetirement()
    {
        WorkoutGameRunner runner;
        QVERIFY(runner.configure(
                WorkoutGameFeatureLab::course(200.0), 200.0, true));
        runner.start(0, 1.0);
        WorkoutGameEngineFrame first;
        QVERIFY(waitForFrame(runner, first));
        QTest::qWait(80);

        WorkoutGameEngineFrame finalFrame;
        QVERIFY(runner.stopAndTakeLatest(
                first.visual.simulation.workoutTimeMs, finalFrame));
        QVERIFY(finalFrame.visual.simulation.workoutTimeMs
                > first.visual.simulation.workoutTimeMs);
        QVERIFY(!runner.takeLatest(finalFrame));
    }

    void failedCourseReplacementCannotLeakPreviousFrame()
    {
        WorkoutGameRunner runner;
        QVERIFY(runner.configure(
                WorkoutGameFeatureLab::course(200.0), 200.0, true));
        runner.start(0, 1.0);
        WorkoutGameEngineFrame previous;
        QVERIFY(waitForFrame(runner, previous));

        WorkoutGameCourse invalid;
        QVERIFY(!runner.configure(invalid, 200.0, false));
        WorkoutGameEngineFrame stale;
        QVERIFY(!runner.takeLatest(stale));
    }

    void repeatedStartStopAndReplacementShutDownCleanly()
    {
        WorkoutGameRunner runner;
        for (int iteration = 0; iteration < 8; ++iteration) {
            QVERIFY(runner.configure(
                    WorkoutGameFeatureLab::course(200.0), 200.0, true));
            WorkoutGameEngineFrame frame;
            runner.start(iteration * 100, 1.0);
            QTest::qWait(25);
            QVERIFY(waitForFrame(runner, frame));
            runner.stop(frame.visual.simulation.workoutTimeMs);
        }
        runner.shutdown();
    }

    void staleTelemetryCannotKeepTheRiderPowered()
    {
        WorkoutGameRunner runner;
        QVERIFY(runner.configure(
                WorkoutGameFeatureLab::course(200.0), 200.0, false));
        WorkoutGameEngineInput input;
        input.simulation.actualWatts = 260.0;
        input.simulation.targetWatts = 220.0;
        input.simulation.cadenceRpm = 92.0;
        input.heartRate = 155;
        input.telemetryMonotonicTimeMs =
                WorkoutGameRunner::monotonicMilliseconds() - 5000;
        runner.setTelemetry(input);
        runner.start(0, 1.0);

        WorkoutGameEngineFrame frame;
        QVERIFY(waitForFrame(runner, frame));
        QVERIFY(frame.telemetryStale);
        QCOMPARE(frame.watts, 0.0);
        QCOMPARE(frame.cadenceRpm, 0);
        QCOMPARE(frame.heartRate, 0);
        QCOMPARE(frame.targetWatts, 220.0);
    }

    void freshTelemetryRemainsAvailableToTheEngine()
    {
        WorkoutGameRunner runner;
        QVERIFY(runner.configure(
                WorkoutGameFeatureLab::course(200.0), 200.0, false));
        WorkoutGameEngineInput input;
        input.simulation.actualWatts = 240.0;
        input.simulation.targetWatts = 210.0;
        input.simulation.cadenceRpm = 89.0;
        input.heartRate = 149;
        input.telemetryMonotonicTimeMs =
                WorkoutGameRunner::monotonicMilliseconds();
        runner.setTelemetry(input);
        runner.start(0, 1.0);

        WorkoutGameEngineFrame frame;
        QVERIFY(waitForFrame(runner, frame));
        QVERIFY(!frame.telemetryStale);
        QCOMPARE(frame.watts, 240.0);
        QCOMPARE(frame.cadenceRpm, 89);
        QCOMPARE(frame.heartRate, 149);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameRunner)
#include "testWorkoutGameRunner.moc"
