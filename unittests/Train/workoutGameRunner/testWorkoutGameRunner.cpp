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

}

class TestWorkoutGameRunner : public QObject
{
    Q_OBJECT

private slots:
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
        runner.setTelemetry(input);
        runner.start(0, 1.0);

        QTest::qWait(130);
        WorkoutGameEngineFrame first;
        QVERIFY(waitForFrame(runner, first));
        QVERIFY(first.visual.simulation.workoutTimeMs >= 80);
        QVERIFY(first.visual.presentationTimeMs > 0);
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
        while (runner.takeLatest(frame)) {}

        runner.pause(beforePause);
        QTest::qWait(80);
        QVERIFY(!runner.takeLatest(frame));

        runner.resume(beforePause, 1.0);
        QVERIFY(waitForFrame(runner, frame));
        QVERIFY(frame.visual.simulation.workoutTimeMs >= beforePause);
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
};

QTEST_GUILESS_MAIN(TestWorkoutGameRunner)
#include "testWorkoutGameRunner.moc"
