/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameFeatureLab.h"
#include "Train/WorkoutGameReplay.h"

#include <QTest>

#include <limits>

namespace {

WorkoutGameReplay featureLabReplay(WorkoutGameFeatureLabScenario scenario)
{
    WorkoutGameReplay replay;
    replay.course = WorkoutGameFeatureLab::course(200.0);
    replay.ftpWatts = 200.0;
    replay.featureLabEnabled = true;
    for (std::int64_t timeMs = 0; timeMs < replay.course.durationMs;
         timeMs += 20) {
        WorkoutGameReplaySample sample;
        sample.input.simulation = WorkoutGameFeatureLab::input(
                replay.course, timeMs, scenario);
        sample.input.heartRate = 138 + int((timeMs / 1000) % 12);
        sample.presentationTimeMs = 250000 + timeMs;
        replay.samples.push_back(sample);
    }
    return replay;
}

}

class TestWorkoutGameReplay : public QObject
{
    Q_OBJECT

private slots:
    void completeReplaysAreBitExactWithinOneBuild()
    {
        const WorkoutGameReplay pass = featureLabReplay(
                WorkoutGameFeatureLabScenario::Pass);
        const WorkoutGameReplay bypass = featureLabReplay(
                WorkoutGameFeatureLabScenario::Bypass);
        const WorkoutGameReplayResult passBaseline =
                WorkoutGameReplayHarness::run(pass);
        const WorkoutGameReplayResult bypassBaseline =
                WorkoutGameReplayHarness::run(bypass);
        QVERIFY(passBaseline.passed);
        QVERIFY(bypassBaseline.passed);
        QVERIFY(passBaseline.finalStateHash != 0);
        QVERIFY(bypassBaseline.finalStateHash != 0);
        QVERIFY(passBaseline.finalStateHash != bypassBaseline.finalStateHash);
        QCOMPARE(passBaseline.frameStateHashes.size(), pass.samples.size());

        for (int repetition = 0; repetition < 12; ++repetition) {
            const WorkoutGameReplayResult result =
                    WorkoutGameReplayHarness::run(
                        repetition & 1 ? bypass : pass);
            QVERIFY(result.passed);
            QCOMPARE(result.finalStateHash,
                     repetition & 1
                        ? bypassBaseline.finalStateHash
                        : passBaseline.finalStateHash);
            QCOMPARE(result.frameStateHashes,
                     repetition & 1
                        ? bypassBaseline.frameStateHashes
                        : passBaseline.frameStateHashes);
        }
    }

    void pauseAndResumeReplayPreservesProgressAndHash()
    {
        WorkoutGameReplay replay;
        replay.course = WorkoutGameFeatureLab::course(200.0);
        replay.ftpWatts = 200.0;
        replay.featureLabEnabled = true;
        std::int64_t presentationTimeMs = 10000;
        for (std::int64_t timeMs = 0; timeMs <= 3000; timeMs += 20) {
            WorkoutGameReplaySample sample;
            sample.input.simulation = WorkoutGameFeatureLab::input(
                    replay.course, timeMs,
                    WorkoutGameFeatureLabScenario::Pass);
            sample.presentationTimeMs = presentationTimeMs;
            replay.samples.push_back(sample);
            presentationTimeMs += 20;
        }
        for (int frame = 0; frame < 15; ++frame) {
            WorkoutGameReplaySample sample = replay.samples.back();
            sample.input.simulation.paused = true;
            sample.presentationTimeMs = presentationTimeMs;
            replay.samples.push_back(sample);
            presentationTimeMs += 20;
        }
        for (std::int64_t timeMs = 3020; timeMs <= 6000; timeMs += 20) {
            WorkoutGameReplaySample sample;
            sample.input.simulation = WorkoutGameFeatureLab::input(
                    replay.course, timeMs,
                    WorkoutGameFeatureLabScenario::Pass);
            sample.presentationTimeMs = presentationTimeMs;
            replay.samples.push_back(sample);
            presentationTimeMs += 20;
        }

        const WorkoutGameReplayResult first =
                WorkoutGameReplayHarness::run(replay);
        const WorkoutGameReplayResult second =
                WorkoutGameReplayHarness::run(replay);
        QVERIFY(first.passed);
        QVERIFY(second.passed);
        QCOMPARE(first.finalStateHash, second.finalStateHash);
        QVERIFY(first.finalFrame.visual.simulation.workoutTimeMs >= 6000);
        QVERIFY(first.finalFrame.visual.world.rider.distanceMeters > 0.0);
    }

    void oneInputMutationChangesTheReplayHash()
    {
        WorkoutGameReplay original = featureLabReplay(
                WorkoutGameFeatureLabScenario::Pass);
        WorkoutGameReplay changed = original;
        changed.samples[changed.samples.size() / 2]
                .input.simulation.actualWatts += 1.0;
        const WorkoutGameReplayResult left =
                WorkoutGameReplayHarness::run(original);
        const WorkoutGameReplayResult right =
                WorkoutGameReplayHarness::run(changed);
        QVERIFY(left.passed);
        QVERIFY(right.passed);
        QVERIFY(left.finalStateHash != right.finalStateHash);
    }

    void rejectsUnsupportedOrInvalidStreams()
    {
        WorkoutGameReplay replay = featureLabReplay(
                WorkoutGameFeatureLabScenario::Pass);
        replay.formatVersion = WorkoutGameReplay::CurrentFormatVersion + 1;
        QCOMPARE(WorkoutGameReplayHarness::run(replay).failure,
                 WorkoutGameReplayFailure::UnsupportedFormat);

        replay = featureLabReplay(WorkoutGameFeatureLabScenario::Pass);
        replay.samples[1].presentationTimeMs =
                replay.samples[0].presentationTimeMs - 1;
        QCOMPARE(WorkoutGameReplayHarness::run(replay).failure,
                 WorkoutGameReplayFailure::PresentationTimeRegression);

        replay = featureLabReplay(WorkoutGameFeatureLabScenario::Pass);
        replay.samples[0].input.simulation.workoutTimeMs = -1;
        QCOMPARE(WorkoutGameReplayHarness::run(replay).failure,
                 WorkoutGameReplayFailure::InvalidInput);

        replay = featureLabReplay(WorkoutGameFeatureLabScenario::Pass);
        replay.samples[2].input.simulation.workoutTimeMs = 10;
        QCOMPARE(WorkoutGameReplayHarness::run(replay).failure,
                 WorkoutGameReplayFailure::WorkoutTimeRegression);

        replay = featureLabReplay(WorkoutGameFeatureLabScenario::Pass);
        replay.samples[1].input.simulation.actualWatts =
                std::numeric_limits<double>::quiet_NaN();
        QCOMPARE(WorkoutGameReplayHarness::run(replay).failure,
                 WorkoutGameReplayFailure::InvalidInput);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameReplay)
#include "testWorkoutGameReplay.moc"
