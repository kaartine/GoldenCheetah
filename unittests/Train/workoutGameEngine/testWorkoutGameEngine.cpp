/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameEngine.h"
#include "Train/WorkoutGameFeatureLab.h"
#include "Train/WorkoutGameRiderVisual.h"
#include "Train/WorkoutGameTabletopGeometry.h"
#include "Train/TrainingDataGenerator.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

void mixValue(std::uint64_t &hash, std::uint64_t value)
{
    constexpr std::uint64_t FnvPrime = 1099511628211ULL;
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffULL;
        hash *= FnvPrime;
    }
}

void mixReal(std::uint64_t &hash, double value)
{
    mixValue(hash, std::uint64_t(std::int64_t(std::llround(value * 1000000.0))));
}

std::uint64_t replayDigest(
        const WorkoutGameCourse &course,
        WorkoutGameFeatureLabScenario scenario)
{
    constexpr double FtpWatts = 200.0;
    constexpr std::uint64_t FnvOffset = 14695981039346656037ULL;
    WorkoutGameEngine engine;
    if (!engine.configure(course, FtpWatts, true)) return 0;

    std::uint64_t digest = FnvOffset;
    for (std::int64_t timeMs = 0; timeMs < course.durationMs; timeMs += 20) {
        WorkoutGameEngineInput input;
        input.simulation = WorkoutGameFeatureLab::input(
                course, timeMs, scenario);
        input.heartRate = scenario == WorkoutGameFeatureLabScenario::Pass
                ? 148 : 122;
        const WorkoutGameEngineFrame frame = engine.update(
                input, 100000 + timeMs);
        const auto &simulation = frame.visual.simulation;
        const auto &world = frame.visual.world;
        const auto &camera = frame.visual.camera;
        mixValue(digest, std::uint64_t(simulation.workoutTimeMs));
        mixValue(digest, std::uint64_t(simulation.activeSection));
        mixValue(digest, simulation.score);
        mixValue(digest, std::uint64_t(simulation.featureOutcome));
        mixValue(digest, std::uint64_t(simulation.route));
        mixReal(digest, simulation.courseProgress);
        mixReal(digest, simulation.sectionProgress);
        mixReal(digest, simulation.speedKph);
        mixReal(digest, simulation.challengeReadiness);
        mixValue(digest, world.generation);
        mixValue(digest, std::uint64_t(world.terrain));
        mixReal(digest, world.rider.distanceMeters);
        mixReal(digest, world.rider.elevationMeters);
        mixReal(digest, world.rider.pitchDegrees);
        mixReal(digest, world.rider.rearSuspension);
        mixReal(digest, world.rider.frontSuspension);
        mixReal(digest, world.rider.clearanceMeters);
        mixValue(digest, world.rider.airborne ? 1 : 0);
        mixValue(digest, world.rider.walking ? 1 : 0);
        mixReal(digest, world.speedMetersPerSecond);
        mixReal(digest, world.landingImpact);
        mixReal(digest, camera.centerDistanceMeters);
        mixReal(digest, camera.centerElevationMeters);
        mixReal(digest, camera.lookAheadMeters);
        mixReal(digest, camera.zoom);
        mixReal(digest, frame.visual.riderPedalCycles);
        mixValue(digest, frame.sequence);
        mixValue(digest, std::uint64_t(frame.heartRate));
    }
    return digest;
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

const WorkoutGameRoadPiece *challengePieceFor(
        const WorkoutGameRoadCourse &road,
        int sourceSectionIndex)
{
    for (const WorkoutGameRoadPiece &piece : road.pieces) {
        if (piece.challenge.enabled
                && int(piece.sourceSectionIndex) == sourceSectionIndex) {
            return &piece;
        }
    }
    return nullptr;
}

WorkoutGameCourse gapJumpCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 1701u;
    course.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::SprintJump;
    section.terrain = WorkoutGameTerrainKind::GapJump;
    section.durationMs = course.durationMs;
    section.lengthMeters = 120.0;
    section.targetWatts = 200.0;
    section.difficulty = 0.5;
    section.challengeCount = 1;
    course.sections.push_back(section);
    return course;
}

}

class TestWorkoutGameEngine : public QObject
{
    Q_OBJECT

private slots:
    void featureLabGapScenariosExerciseEveryLineAndPowerGate()
    {
        constexpr double FtpWatts = 200.0;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);
        struct Case {
            WorkoutGameFeatureLabGapScenario scenario;
            WorkoutGameGapJumpLine expectedLine;
            WorkoutGameFeatureOutcome expectedOutcome;
        };
        const Case cases[] = {
            {WorkoutGameFeatureLabGapScenario::Short,
             WorkoutGameGapJumpLine::Short,
             WorkoutGameFeatureOutcome::Completed},
            {WorkoutGameFeatureLabGapScenario::Medium,
             WorkoutGameGapJumpLine::Medium,
             WorkoutGameFeatureOutcome::Completed},
            {WorkoutGameFeatureLabGapScenario::Long,
             WorkoutGameGapJumpLine::Long,
             WorkoutGameFeatureOutcome::Completed},
            {WorkoutGameFeatureLabGapScenario::Safe,
             WorkoutGameGapJumpLine::None,
             WorkoutGameFeatureOutcome::Bypassed}
        };

        for (const Case &testCase : cases) {
            WorkoutGameEngine engine;
            QVERIFY(engine.configure(course, FtpWatts, true));
            bool observedLock = false;
            for (std::int64_t timeMs = 0; timeMs < course.durationMs;
                 timeMs += 20) {
                WorkoutGameEngineInput input;
                input.simulation = WorkoutGameFeatureLab::input(
                        course, timeMs, WorkoutGameFeatureLabScenario::Pass);
                WorkoutGameFeatureLab::applyGapScenario(
                        course, timeMs, testCase.scenario, input.simulation);
                const WorkoutGameEngineFrame frame = engine.update(
                        input, 100000 + timeMs);
                if (frame.visual.feature.terrain
                            != WorkoutGameTerrainKind::GapJump
                        || !frame.visual.feature.gapLineLocked) {
                    continue;
                }

                observedLock = true;
                QCOMPARE(frame.visual.feature.lockedGapLine,
                         testCase.expectedLine);
                QCOMPARE(frame.visual.feature.outcome,
                         testCase.expectedOutcome);
                QCOMPARE(frame.visual.simulation.featureOutcome,
                         testCase.expectedOutcome);
                QCOMPARE(frame.visual.feature.launchSpeedReady, true);
                QCOMPARE(frame.visual.feature.launchPowerReady,
                         testCase.expectedOutcome
                            == WorkoutGameFeatureOutcome::Completed);
            }
            QVERIFY2(observedLock, "gap scenario never reached line lock");
        }
    }

    void gapJumpUsesSectionTargetAndPublishesOneCommittedOutcome()
    {
        const WorkoutGameCourse course = gapJumpCourse();
        WorkoutGameEngine engine;
        QVERIFY(engine.configure(course, 200.0, false));

        bool observedLock = false;
        std::uint64_t committedScore = 0;
        for (std::int64_t timeMs = 0; timeMs < course.durationMs;
             timeMs += 20) {
            WorkoutGameEngineInput input;
            input.simulation.workoutTimeMs = timeMs;
            input.simulation.actualWatts = 200.0;
            input.simulation.targetWatts = 0.0;
            input.simulation.cadenceRpm = 85.0;
            input.simulation.authoritativeSpeedKph = 25.2;
            const WorkoutGameEngineFrame frame =
                    engine.update(input, timeMs);
            if (!frame.visual.feature.gapLineLocked) continue;

            observedLock = true;
            QCOMPARE(frame.visual.feature.lockedGapLine,
                     WorkoutGameGapJumpLine::Long);
            QCOMPARE(frame.visual.feature.outcome,
                     WorkoutGameFeatureOutcome::Completed);
            QCOMPARE(frame.visual.simulation.featureOutcome,
                     WorkoutGameFeatureOutcome::Completed);
            QCOMPARE(frame.visual.simulation.route,
                     WorkoutGameRoute::MainLine);
            QVERIFY(frame.visual.simulation.score >= committedScore);
            committedScore = frame.visual.simulation.score;
        }

        QVERIFY(observedLock);
        QVERIFY(committedScore > 0);
    }

    void completeFeatureReplayIsDeterministicFiniteAndForwardOnly()
    {
        constexpr double FtpWatts = 200.0;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);
        const int expectedFeatureCount = int(std::count_if(
                course.sections.begin(), course.sections.end(),
                [](const WorkoutGameSection &section) {
                    return section.challengeCount > 0;
                }));
        WorkoutGameEngine first;
        WorkoutGameEngine second;
        QVERIFY(first.configure(course, FtpWatts, true));
        QVERIFY(second.configure(course, FtpWatts, true));

        double priorDistance = 0.0;
        WorkoutGameAudioCueTracker audioTracker;
        int featureCueCount = 0;
        int landingCueCount = 0;
        for (std::int64_t timeMs = 0; timeMs < course.durationMs;
             timeMs += 20) {
            WorkoutGameEngineInput input;
            input.simulation = WorkoutGameFeatureLab::input(
                    course, timeMs, WorkoutGameFeatureLabScenario::Pass);
            input.heartRate = 148;
            const WorkoutGameEngineFrame left = first.update(
                    input, 100000 + timeMs);
            const WorkoutGameEngineFrame right = second.update(
                    input, 100000 + timeMs);

            QVERIFY(left.visual.simulation.ready);
            QVERIFY(left.visual.world.ready);
            QVERIFY(std::isfinite(left.visual.world.rider.distanceMeters));
            QVERIFY(std::isfinite(left.visual.world.rider.elevationMeters));
            QVERIFY(std::isfinite(left.visual.world.speedMetersPerSecond));
            QVERIFY(std::isfinite(left.visual.riderPedalCycles));
            QVERIFY2(left.visual.world.rider.distanceMeters + 1e-7
                            >= priorDistance,
                     qPrintable(QStringLiteral(
                         "the canonical rider moved backward at %1 ms: %2 -> %3")
                         .arg(timeMs)
                         .arg(priorDistance, 0, 'f', 9)
                         .arg(left.visual.world.rider.distanceMeters,
                              0, 'f', 9)));
            QCOMPARE(left.visual.simulation.workoutTimeMs,
                     right.visual.simulation.workoutTimeMs);
            QCOMPARE(left.visual.world.rider.distanceMeters,
                     right.visual.world.rider.distanceMeters);
            QCOMPARE(left.visual.world.rider.elevationMeters,
                     right.visual.world.rider.elevationMeters);
            QCOMPARE(left.visual.world.rider.pitchDegrees,
                     right.visual.world.rider.pitchDegrees);
            QCOMPARE(left.visual.riderPedalCycles,
                     right.visual.riderPedalCycles);
            QCOMPARE(left.audioEvents.count, right.audioEvents.count);
            QCOMPARE(left.audioEvents.epoch, right.audioEvents.epoch);
            const WorkoutGameAudioEvents audio = audioTracker.update(
                    left.audioEvents);
            if (audio.feature) ++featureCueCount;
            if (audio.landing) ++landingCueCount;
            priorDistance = left.visual.world.rider.distanceMeters;
        }
        QVERIFY(priorDistance > 100.0);
        QCOMPARE(featureCueCount, expectedFeatureCount);
        QVERIFY(landingCueCount > 0);
    }

    void completeReplayDigestIsStableAcrossTwentyRuns()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const std::uint64_t expected = replayDigest(
                course, WorkoutGameFeatureLabScenario::Pass);
        QVERIFY(expected != 0);
        for (int run = 1; run < 20; ++run) {
            QCOMPARE(replayDigest(
                    course, WorkoutGameFeatureLabScenario::Pass), expected);
        }
        QVERIFY(replayDigest(course, WorkoutGameFeatureLabScenario::Bypass)
                != expected);
    }

    void dataGeneratorDeterministicallyCompletesAndBypassesEveryFeature()
    {
        constexpr double FtpWatts = 190.0;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);

        const auto runScenario = [&](TrainingDataGeneratorMode mode,
                                     WorkoutGameFeatureOutcome expected) {
            TrainingDataGenerator generator;
            generator.setMode(mode);
            WorkoutGameEngine engine;
            QVERIFY(engine.configure(course, FtpWatts, true));
            std::vector<bool> observed(course.sections.size(), false);
            double maximumMainLineLateralMeters = 0.0;
            double maximumGapLineLateralMeters = 0.0;
            double maximumTraceLateralStepMeters = 0.0;
            double priorTraceLateralMeters = 0.0;
            bool hasPriorTraceLateral = false;
            std::int64_t nextTraceTimeMs = 0;

            for (std::int64_t timeMs = 0; timeMs < course.durationMs;
                 timeMs += 20) {
                WorkoutGameEngineInput input;
                const WorkoutGameFeatureLabScenario labScenario =
                        expected == WorkoutGameFeatureOutcome::Completed
                        ? WorkoutGameFeatureLabScenario::Pass
                        : WorkoutGameFeatureLabScenario::Bypass;
                input.simulation = WorkoutGameFeatureLab::input(
                        course, timeMs, labScenario);
                generator.setTargetWatts(input.simulation.targetWatts);
                const TrainingDataGeneratorSample sample =
                        generator.nextSample();
                // RealtimeData stores watts as an integer in the live path.
                input.simulation.actualWatts = std::floor(sample.watts);
                input.simulation.cadenceRpm = sample.cadence;
                input.simulation.authoritativeSpeedKph = -1.0;
                input.heartRate = int(std::lround(sample.heartRate));

                const WorkoutGameEngineFrame frame = engine.update(
                        input, 100000 + timeMs);
                if (frame.visual.feature.terrain
                            == WorkoutGameTerrainKind::GapJump
                        && frame.visual.feature.gapLineLocked) {
                    QCOMPARE(frame.visual.simulation.featureOutcome,
                             frame.visual.feature.outcome);
                    QCOMPARE(frame.visual.simulation.route,
                             frame.visual.feature.route);
                    QCOMPARE(frame.visual.simulation.challengeReadiness,
                             frame.visual.feature.readiness);
                }
                const int section = frame.visual.simulation.activeSection;
                if (section >= 0
                        && section < int(course.sections.size())
                        && course.sections[std::size_t(section)].challengeCount > 0
                        && frame.visual.simulation.featureOutcome == expected) {
                    observed[std::size_t(section)] = true;
                }
                if (expected == WorkoutGameFeatureOutcome::Completed) {
                    const double lateral = std::abs(
                            frame.visual.feature.lateralOffsetMeters);
                    if (frame.visual.feature.terrain
                            == WorkoutGameTerrainKind::GapJump) {
                        maximumGapLineLateralMeters = std::max(
                                maximumGapLineLateralMeters, lateral);
                    } else {
                        maximumMainLineLateralMeters = std::max(
                                maximumMainLineLateralMeters, lateral);
                    }
                    const WorkoutGameRiderVisualPose pose =
                            WorkoutGameRiderVisual::pose(
                                frame.visual.world,
                                frame.visual.feature,
                                frame.watts);
                    if (pose.airborne && pose.airHeightMeters >= 0.08) {
                        QVERIFY2(
                            WorkoutGameFeatureRuntime::airborneExpected(
                                frame.visual.feature),
                            qPrintable(QStringLiteral(
                                "unexpected air at %1 ms in section %2")
                                .arg(timeMs).arg(section)));
                    }
                }
                if (timeMs >= nextTraceTimeMs) {
                    const double lateral =
                            frame.visual.feature.lateralOffsetMeters;
                    if (hasPriorTraceLateral) {
                        maximumTraceLateralStepMeters = std::max(
                                maximumTraceLateralStepMeters,
                                std::abs(lateral
                                    - priorTraceLateralMeters));
                    }
                    priorTraceLateralMeters = lateral;
                    hasPriorTraceLateral = true;
                    nextTraceTimeMs += 250;
                }
            }

            int observedFeatures = 0;
            for (std::size_t section = 0; section < course.sections.size();
                 ++section) {
                if (course.sections[section].challengeCount <= 0) continue;
                QVERIFY2(observed[section], qPrintable(QStringLiteral(
                    "generator mode %1 did not produce outcome %2 for section %3")
                    .arg(int(mode)).arg(int(expected)).arg(section)));
                ++observedFeatures;
            }
            QCOMPARE(observedFeatures, int(std::count_if(
                    course.sections.begin(), course.sections.end(),
                    [](const WorkoutGameSection &section) {
                        return section.challengeCount > 0;
                    })));
            if (expected == WorkoutGameFeatureOutcome::Completed) {
                QVERIFY(maximumMainLineLateralMeters < 0.01);
                QVERIFY(maximumGapLineLateralMeters <= 2.5);
            } else {
                QVERIFY2(maximumTraceLateralStepMeters < 1.0,
                         qPrintable(QStringLiteral(
                             "safe line moved %1 m in one trace interval")
                             .arg(maximumTraceLateralStepMeters)));
            }
        };

        runScenario(TrainingDataGeneratorMode::OnTarget,
                    WorkoutGameFeatureOutcome::Completed);
        runScenario(TrainingDataGeneratorMode::UnderTarget,
                    WorkoutGameFeatureOutcome::Bypassed);
    }

    void enginePreservesTheAuthoritativeInputTargetInFeatureLab()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameEngine engine;
        QVERIFY(engine.configure(course, 200.0, true));

        WorkoutGameEngineInput input;
        input.simulation = WorkoutGameFeatureLab::input(
                course, 0, WorkoutGameFeatureLabScenario::Pass);
        input.simulation.actualWatts = 137.0;
        input.simulation.targetWatts = 137.0;
        const WorkoutGameEngineFrame frame = engine.update(input, 100000);

        QCOMPARE(frame.targetWatts, 137.0);
    }

    void skippedTimeResynchronizesWithoutCatchupSimulation()
    {
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        QVERIFY(engine.configure(course, 200.0, true));
        WorkoutGameEngineInput input;
        input.simulation = WorkoutGameFeatureLab::input(
                course, 0, WorkoutGameFeatureLabScenario::Pass);
        const WorkoutGameEngineFrame before = engine.update(input, 1000);

        input.simulation = WorkoutGameFeatureLab::input(
                course, 5000, WorkoutGameFeatureLabScenario::Pass);
        engine.resynchronize(input, 6000, 250);
        input.simulation.workoutTimeMs = 5020;
        const WorkoutGameEngineFrame after = engine.update(input, 6020, 250);
        const WorkoutGameRoadTimelineSample road =
                WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(
                    WorkoutGameRoadCourseBuilder::build(course, 200.0), 5020);

        QCOMPARE(after.visual.simulation.droppedCatchupMs, std::int64_t(0));
        QCOMPARE(after.skippedTicks, std::size_t(250));
        QVERIFY(road.ready);
        QCOMPARE(after.visual.world.rider.distanceMeters,
                 road.distanceMeters);
        QVERIFY(after.visual.riderPedalCycles
                - before.visual.riderPedalCycles < 0.2);
    }

    void nonFiniteTelemetryCannotPoisonPublishedFrame()
    {
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        QVERIFY(engine.configure(course, 200.0, false));
        WorkoutGameEngineInput input;
        input.simulation.workoutTimeMs = 0;
        input.simulation.actualWatts =
                std::numeric_limits<double>::quiet_NaN();
        input.simulation.targetWatts =
                std::numeric_limits<double>::infinity();
        input.simulation.cadenceRpm =
                std::numeric_limits<double>::infinity();
        input.simulation.authoritativeSpeedKph =
                std::numeric_limits<double>::quiet_NaN();
        input.simulation.drivetrainSpeedLimitKph =
                std::numeric_limits<double>::infinity();

        const WorkoutGameEngineFrame frame = engine.update(input, 1000);
        QVERIFY(frame.visual.simulation.ready);
        QVERIFY(std::isfinite(frame.visual.riderPedalCycles));
        QVERIFY(std::isfinite(frame.watts));
        QVERIFY(std::isfinite(frame.targetWatts));
        QCOMPARE(frame.cadenceRpm, 0);
    }

    void extremeFiniteTelemetryIsBoundedBeforeSimulation()
    {
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        QVERIFY(engine.configure(course, 200.0, false));
        WorkoutGameEngineInput input;
        input.simulation.workoutTimeMs = 0;
        input.simulation.actualWatts = std::numeric_limits<double>::max();
        input.simulation.targetWatts = std::numeric_limits<double>::max();
        input.simulation.cadenceRpm = std::numeric_limits<double>::max();
        input.simulation.authoritativeSpeedKph =
                std::numeric_limits<double>::max();
        input.simulation.drivetrainSpeedLimitKph =
                std::numeric_limits<double>::max();
        input.heartRate = std::numeric_limits<int>::max();

        const WorkoutGameEngineFrame frame = engine.update(input, 1000);
        QVERIFY(frame.visual.simulation.ready);
        QVERIFY(std::isfinite(frame.visual.riderPedalCycles));
        QCOMPARE(frame.watts, 10000.0);
        QCOMPARE(frame.targetWatts, 10000.0);
        QCOMPARE(frame.cadenceRpm, 300);
        QCOMPARE(frame.heartRate, 300);
    }

    void framePublishesOneAuthoritativeRoadAndFeaturePosition()
    {
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        QVERIFY(engine.configure(course, 200.0, true));
        WorkoutGameEngineInput input;
        input.simulation = WorkoutGameFeatureLab::input(
                course, 5000, WorkoutGameFeatureLabScenario::Pass);

        const WorkoutGameEngineFrame frame = engine.update(input, 6000);
        const WorkoutGameRoadTimelineSample expected =
                WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(road, 5000);
        QVERIFY(expected.ready);
        QVERIFY(frame.visual.feature.ready);
        QCOMPARE(frame.visual.world.rider.distanceMeters,
                 expected.distanceMeters);
        QCOMPARE(frame.visual.feature.visualDistanceMeters,
                 expected.distanceMeters);
        QCOMPARE(frame.visual.feature.sourceSectionIndex,
                 int(expected.sourceSectionIndex));
    }

    void completedLogFeatureProducesAVisibleAirborneArc()
    {
        constexpr double FtpWatts = 200.0;
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);
        QVERIFY(engine.configure(course, FtpWatts, true));

        double maximumAirHeightMeters = 0.0;
        double maximumLiftPixels = 0.0;
        int airborneFrames = 0;
        for (std::int64_t timeMs = 0;
             timeMs < course.sections.front().durationMs;
             timeMs += 20) {
            WorkoutGameEngineInput input;
            input.simulation = WorkoutGameFeatureLab::input(
                    course, timeMs, WorkoutGameFeatureLabScenario::Pass);
            // Keep drivetrain speed far below the 7 m/s course timeline so
            // landing still proves that the timeline owns flight calibration.
            input.simulation.authoritativeSpeedKph = 12.0;
            const WorkoutGameEngineFrame frame = engine.update(
                    input, 100000 + timeMs);
            maximumAirHeightMeters = std::max(
                    maximumAirHeightMeters,
                    frame.visual.world.rider.airHeightMeters());
            airborneFrames += frame.visual.world.rider.airborne ? 1 : 0;
            const WorkoutGameRiderVisualPose pose =
                    WorkoutGameRiderVisual::pose(
                        frame.visual.world, frame.visual.feature, 188.0);
            maximumLiftPixels = std::max(
                    maximumLiftPixels, pose.liftPixels);
        }

        QVERIFY2(maximumAirHeightMeters >= 0.20,
                 qPrintable(QStringLiteral(
                     "feature reached only %1 m air height")
                     .arg(maximumAirHeightMeters)));
        QVERIFY(airborneFrames >= 8);
        QVERIFY2(maximumLiftPixels >= 65.0,
                 qPrintable(QStringLiteral(
                     "feature produced only %1 px of visible lift")
                     .arg(maximumLiftPixels)));
    }

    void completedTabletopProducesReadableLiftAndDepthCue()
    {
        constexpr double FtpWatts = 200.0;
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const int tabletopSection = sectionForTerrain(
                course, WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadPiece *tabletop = challengePieceFor(
                road, tabletopSection);
        QVERIFY(tabletopSection >= 0);
        QVERIFY(tabletop != nullptr);
        QVERIFY(engine.configure(course, FtpWatts, true));
        const WorkoutGameTabletopGeometryProfile profile =
                WorkoutGameTabletopGeometry::profile(tabletop->difficulty);

        double maximumLiftPixels = 0.0;
        double maximumAirHeightMeters = 0.0;
        double maximumAirHeightStepMeters = 0.0;
        double airHeightBeforeMaximumStepMeters = 0.0;
        double airHeightAfterMaximumStepMeters = 0.0;
        std::int64_t maximumAirHeightStepTimeMs = 0;
        double minimumAirborneShadowScale = 1.0;
        int readableAirborneFrames = 0;
        int consecutiveAirborneFrames = 0;
        int maximumConsecutiveAirborneFrames = 0;
        double priorAirHeightMeters = 0.0;
        bool hasPriorAirHeight = false;
        bool priorAirborne = false;
        bool observedLanding = false;
        double landingLocalDistanceMeters = 0.0;
        for (std::int64_t timeMs = 0; timeMs < course.durationMs;
             timeMs += 20) {
            WorkoutGameEngineInput input;
            input.simulation = WorkoutGameFeatureLab::input(
                    course, timeMs, WorkoutGameFeatureLabScenario::Pass);
            const WorkoutGameEngineFrame frame = engine.update(
                    input, 100000 + timeMs);
            if (frame.visual.simulation.activeSection != tabletopSection) {
                continue;
            }
            const WorkoutGameRiderVisualPose pose =
                    WorkoutGameRiderVisual::pose(
                        frame.visual.world, frame.visual.feature, 188.0);
            maximumAirHeightMeters = std::max(
                    maximumAirHeightMeters, pose.airHeightMeters);
            if (hasPriorAirHeight) {
                const double step = std::abs(
                        pose.airHeightMeters - priorAirHeightMeters);
                if (step > maximumAirHeightStepMeters) {
                    maximumAirHeightStepMeters = step;
                    airHeightBeforeMaximumStepMeters = priorAirHeightMeters;
                    airHeightAfterMaximumStepMeters = pose.airHeightMeters;
                    maximumAirHeightStepTimeMs = timeMs;
                }
            }
            priorAirHeightMeters = pose.airHeightMeters;
            hasPriorAirHeight = true;
            consecutiveAirborneFrames = pose.airborne
                    ? consecutiveAirborneFrames + 1 : 0;
            maximumConsecutiveAirborneFrames = std::max(
                    maximumConsecutiveAirborneFrames,
                    consecutiveAirborneFrames);
            maximumLiftPixels = std::max(maximumLiftPixels, pose.liftPixels);
            if (pose.airborne && pose.liftPixels >= 25.0) {
                ++readableAirborneFrames;
                minimumAirborneShadowScale = std::min(
                        minimumAirborneShadowScale, pose.shadowScale);
            }
            if (priorAirborne && !frame.visual.world.rider.airborne) {
                const double physicalDistanceMeters =
                        frame.visual.world.rider.distanceMeters
                        + frame.visual.world.terrainOffsetMeters;
                const double candidateLandingLocalMeters = physicalDistanceMeters
                        - tabletop->challenge.obstacleDistanceMeters;
                if (candidateLandingLocalMeters
                            >= profile.deckEndMeters - 0.15
                        && candidateLandingLocalMeters
                            <= profile.endMeters + 0.25) {
                    landingLocalDistanceMeters = candidateLandingLocalMeters;
                    observedLanding = true;
                }
            }
            priorAirborne = frame.visual.world.rider.airborne;
        }

        QVERIFY2(maximumLiftPixels >= 28.0,
                 qPrintable(QStringLiteral(
                     "tabletop produced only %1 px of visible lift")
                     .arg(maximumLiftPixels)));
        QVERIFY2(readableAirborneFrames >= 8,
                 qPrintable(QStringLiteral(
                     "tabletop produced only %1 readable airborne frames")
                     .arg(readableAirborneFrames)));
        QVERIFY(maximumAirHeightMeters <= 1.8);
        QVERIFY2(maximumConsecutiveAirborneFrames <= 100,
                 qPrintable(QStringLiteral(
                     "tabletop remained airborne for %1 ms")
                     .arg(maximumConsecutiveAirborneFrames * 20)));
        QVERIFY2(maximumAirHeightStepMeters <= 0.25,
                 qPrintable(QStringLiteral(
                     "tabletop air height stepped by %1 m at %2 ms (%3 -> %4)")
                     .arg(maximumAirHeightStepMeters)
                     .arg(maximumAirHeightStepTimeMs)
                     .arg(airHeightBeforeMaximumStepMeters)
                     .arg(airHeightAfterMaximumStepMeters)));
        QVERIFY(minimumAirborneShadowScale <= 0.84);
        QVERIFY(observedLanding);
        QVERIFY(landingLocalDistanceMeters >= profile.deckEndMeters - 0.15);
        QVERIFY2(landingLocalDistanceMeters <= profile.endMeters + 0.25,
                 qPrintable(QStringLiteral(
                     "tabletop landed at local %1 m after profile end %2 m")
                     .arg(landingLocalDistanceMeters)
                     .arg(profile.endMeters)));
    }

    void tabletopEngineGatesLaunchOutsideTheCalibratedSpeedRange()
    {
        constexpr double FtpWatts = 200.0;
        const auto verifyNoLaunch = [FtpWatts](
                double timelineSpeedMetersPerSecond) {
            WorkoutGameCourse course;
            course.status = WorkoutGameCourseStatus::Ready;
            course.seed = 1771u;
            WorkoutGameSection section;
            section.feature = WorkoutGameFeature::SprintJump;
            section.terrain = WorkoutGameTerrainKind::Tabletop;
            section.lengthMeters = 60.0;
            section.durationMs = std::int64_t(std::llround(
                    section.lengthMeters / timelineSpeedMetersPerSecond
                        * 1000.0));
            section.targetWatts = 230.0;
            section.difficulty = 0.68;
            section.challengeCount = 1;
            course.durationMs = section.durationMs;
            course.sections = {section};

            WorkoutGameEngine engine;
            if (!engine.configure(course, FtpWatts, false)) return false;
            bool observedCompletedAction = false;
            for (std::int64_t timeMs = 0; timeMs < course.durationMs;
                 timeMs += 20) {
                WorkoutGameEngineInput input;
                input.simulation.workoutTimeMs = timeMs;
                input.simulation.actualWatts = section.targetWatts;
                input.simulation.targetWatts = section.targetWatts;
                input.simulation.cadenceRpm = 90.0;
                input.simulation.authoritativeSpeedKph = 20.0;
                const WorkoutGameEngineFrame frame = engine.update(
                        input, 100000 + timeMs);
                if (frame.visual.feature.phase
                            == WorkoutGameFeaturePhase::Action
                        && frame.visual.feature.outcome
                            == WorkoutGameFeatureOutcome::Completed) {
                    observedCompletedAction = true;
                    if (frame.visual.feature.triggerJump) return false;
                }
            }
            return observedCompletedAction;
        };

        QVERIFY(verifyNoLaunch(2.5));
        QVERIFY(verifyNoLaunch(10.0));
    }

    void tabletopBypassEntersAtTheBranchWithoutLateralTeleport()
    {
        constexpr double FtpWatts = 200.0;
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const int tabletopSection = sectionForTerrain(
                course, WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadPiece *tabletop = challengePieceFor(
                road, tabletopSection);
        QVERIFY(tabletopSection >= 0);
        QVERIFY(tabletop != nullptr);
        QVERIFY(engine.configure(course, FtpWatts, true));

        bool enteredBypass = false;
        double offsetAtEntry = 0.0;
        double maximumOffsetStep = 0.0;
        double priorOffset = 0.0;
        bool hasPriorOffset = false;
        for (std::int64_t timeMs = 0; timeMs < course.durationMs;
             timeMs += 20) {
            WorkoutGameEngineInput input;
            input.simulation = WorkoutGameFeatureLab::input(
                    course, timeMs, WorkoutGameFeatureLabScenario::Bypass);
            const WorkoutGameEngineFrame frame = engine.update(
                    input, 100000 + timeMs);
            if (frame.visual.simulation.activeSection != tabletopSection) {
                continue;
            }
            const double offset = frame.visual.feature.lateralOffsetMeters;
            if (hasPriorOffset) {
                maximumOffsetStep = std::max(
                        maximumOffsetStep, std::abs(offset - priorOffset));
            }
            if (!enteredBypass
                    && frame.visual.feature.route
                        == WorkoutGameRoute::SafeBypass) {
                enteredBypass = true;
                offsetAtEntry = offset;
                const double physicalDistanceMeters =
                        frame.visual.world.rider.distanceMeters
                        + frame.visual.world.terrainOffsetMeters;
                QVERIFY(physicalDistanceMeters
                        >= tabletop->challenge.bypassStartDistanceMeters);
                QVERIFY(physicalDistanceMeters
                        < tabletop->challenge.bypassStartDistanceMeters
                            + 0.25);
            }
            priorOffset = offset;
            hasPriorOffset = true;
        }

        QVERIFY(enteredBypass);
        QVERIFY(std::abs(offsetAtEntry) < 0.01);
        QVERIFY2(maximumOffsetStep < 0.08,
                 qPrintable(QStringLiteral(
                     "tabletop bypass moved laterally by %1 m in one frame")
                     .arg(maximumOffsetStep)));
    }

    void bypassCannotActivateTheScriptedAirbornePose()
    {
        WorkoutGameWorldSnapshot world;
        WorkoutGameFeatureRuntimeSnapshot feature;
        feature.ready = true;
        feature.outcome = WorkoutGameFeatureOutcome::Bypassed;
        feature.route = WorkoutGameRoute::SafeBypass;
        feature.verticalOffsetMeters = 1.35;

        const WorkoutGameRiderVisualPose pose =
                WorkoutGameRiderVisual::pose(world, feature, 188.0);
        QVERIFY(!pose.airborne);
        QCOMPARE(pose.airHeightMeters, 0.0);
        QCOMPARE(pose.liftPixels, 0.0);
        QCOMPARE(pose.shadowScale, 1.0);
    }

    void physicsSnapshotOwnsAirHeightOverScriptedFeatureArc()
    {
        WorkoutGameWorldSnapshot world;
        world.ready = true;
        world.rider.airborne = true;
        world.rider.clearanceMeters = 0.82 + 0.37;
        WorkoutGameFeatureRuntimeSnapshot feature;
        feature.ready = true;
        feature.outcome = WorkoutGameFeatureOutcome::Completed;
        feature.route = WorkoutGameRoute::MainLine;
        feature.verticalOffsetMeters = 1.35;

        const WorkoutGameRiderVisualPose pose =
                WorkoutGameRiderVisual::pose(world, feature, 188.0);
        QVERIFY(pose.airborne);
        QVERIFY(std::abs(pose.airHeightMeters - 0.37) < 1e-9);
    }

    void generatedThirtySecondEffortProducesAVisibleJump()
    {
        constexpr double FtpWatts = 190.0;
        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build({
            {0, 60000, 140.0, 140.0},
            {60000, 30000, 250.0, 250.0},
            {90000, 60000, 120.0, 120.0}
        }, FtpWatts, 1234u);
        QCOMPARE(course.sections[1].feature,
                 WorkoutGameFeature::SprintJump);

        WorkoutGameEngine engine;
        QVERIFY(engine.configure(course, FtpWatts, false));
        double maximumAirHeightMeters = 0.0;
        int airborneFrames = 0;
        for (std::int64_t timeMs = 0; timeMs < course.durationMs;
             timeMs += 20) {
            const WorkoutGameSection &section = timeMs < 60000
                    ? course.sections[0]
                    : timeMs < 90000
                    ? course.sections[1] : course.sections[2];
            WorkoutGameEngineInput input;
            input.simulation.workoutTimeMs = timeMs;
            input.simulation.actualWatts = section.targetWatts * 1.2;
            input.simulation.targetWatts = section.targetWatts;
            input.simulation.cadenceRpm = 90.0;
            input.simulation.authoritativeSpeedKph = 20.0;
            const WorkoutGameEngineFrame frame = engine.update(
                    input, 100000 + timeMs);
            if (frame.visual.simulation.activeSection == 1) {
                maximumAirHeightMeters = std::max(
                        maximumAirHeightMeters,
                        frame.visual.world.rider.airHeightMeters());
                airborneFrames += frame.visual.world.rider.airborne ? 1 : 0;
            }
        }

        QVERIFY2(maximumAirHeightMeters >= 0.20,
                 qPrintable(QStringLiteral(
                     "generated feature reached only %1 m air height")
                     .arg(maximumAirHeightMeters)));
        QVERIFY(airborneFrames >= 8);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameEngine)
#include "testWorkoutGameEngine.moc"
