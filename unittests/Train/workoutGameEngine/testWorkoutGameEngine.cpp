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

#include <QTest>

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

}

class TestWorkoutGameEngine : public QObject
{
    Q_OBJECT

private slots:
    void completeFeatureReplayIsDeterministicFiniteAndForwardOnly()
    {
        constexpr double FtpWatts = 200.0;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);
        WorkoutGameEngine first;
        WorkoutGameEngine second;
        QVERIFY(first.configure(course, FtpWatts, true));
        QVERIFY(second.configure(course, FtpWatts, true));

        double priorDistance = 0.0;
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
            priorDistance = left.visual.world.rider.distanceMeters;
        }
        QVERIFY(priorDistance > 100.0);
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
        int airborneFrames = 0;
        for (std::int64_t timeMs = 0;
             timeMs < course.sections.front().durationMs;
             timeMs += 20) {
            WorkoutGameEngineInput input;
            input.simulation = WorkoutGameFeatureLab::input(
                    course, timeMs, WorkoutGameFeatureLabScenario::Pass);
            const WorkoutGameEngineFrame frame = engine.update(
                    input, 100000 + timeMs);
            maximumAirHeightMeters = std::max(
                    maximumAirHeightMeters,
                    frame.visual.world.rider.airHeightMeters());
            airborneFrames += frame.visual.world.rider.airborne ? 1 : 0;
        }

        QVERIFY2(maximumAirHeightMeters >= 0.20,
                 qPrintable(QStringLiteral(
                     "feature reached only %1 m air height")
                     .arg(maximumAirHeightMeters)));
        QVERIFY(airborneFrames >= 8);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameEngine)
#include "testWorkoutGameEngine.moc"
