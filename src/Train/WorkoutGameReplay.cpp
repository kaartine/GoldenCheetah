/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameReplay.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {

constexpr std::uint64_t HashOffset = 14695981039346656037ull;
constexpr std::uint64_t HashPrime = 1099511628211ull;

class StateHasher
{
public:
    explicit StateHasher(std::uint64_t seed) : value(seed) {}

    void addBool(bool item) { addUnsigned<std::uint8_t>(item ? 1u : 0u); }

    template<typename Value>
    void addInteger(Value item)
    {
        using Unsigned = typename std::make_unsigned<Value>::type;
        addUnsigned<Unsigned>(static_cast<Unsigned>(item));
    }

    template<typename Enum>
    void addEnum(Enum item)
    {
        addInteger(static_cast<typename std::underlying_type<Enum>::type>(item));
    }

    void addDouble(double item)
    {
        if (item == 0.0) item = 0.0;
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(item), "unexpected double size");
        std::memcpy(&bits, &item, sizeof(bits));
        addUnsigned(bits);
    }

    std::uint64_t result() const { return value; }

private:
    template<typename Unsigned>
    void addUnsigned(Unsigned item)
    {
        for (std::size_t byte = 0; byte < sizeof(Unsigned); ++byte) {
            value ^= std::uint8_t(item & Unsigned(0xff));
            value *= HashPrime;
            item >>= 8;
        }
    }

    std::uint64_t value;
};

void addChallenge(
        StateHasher &hash,
        const WorkoutGameFeatureChallengeProfile &challenge)
{
    hash.addBool(challenge.enabled);
    hash.addEnum(challenge.cue);
    hash.addDouble(challenge.measurementStartProgress);
    hash.addDouble(challenge.decisionProgress);
    hash.addDouble(challenge.minimumEffortRatio);
    hash.addDouble(challenge.minimumCadenceRpm);
    hash.addDouble(challenge.minimumSpeedKph);
    hash.addDouble(challenge.maximumSpeedKph);
    hash.addDouble(challenge.minimumAdherence);
    hash.addInteger(challenge.bonusPoints);
}

void addSimulation(
        StateHasher &hash,
        const WorkoutGameSimulationSnapshot &simulation)
{
    hash.addBool(simulation.ready);
    hash.addBool(simulation.finished);
    hash.addInteger(simulation.workoutTimeMs);
    hash.addInteger(simulation.droppedCatchupMs);
    hash.addInteger(simulation.activeSection);
    hash.addDouble(simulation.courseProgress);
    hash.addDouble(simulation.sectionProgress);
    hash.addDouble(simulation.speedKph);
    hash.addDouble(simulation.adherence);
    hash.addInteger(simulation.score);
    hash.addDouble(simulation.streakSeconds);
    hash.addEnum(simulation.featureOutcome);
    hash.addInteger(simulation.previousFeatureSection);
    hash.addEnum(simulation.previousFeatureOutcome);
    hash.addDouble(simulation.previousFeatureReadiness);
    hash.addEnum(simulation.route);
    addChallenge(hash, simulation.challenge);
    hash.addDouble(simulation.challengeMetrics.averageActualWatts);
    hash.addDouble(simulation.challengeMetrics.averageTargetWatts);
    hash.addDouble(simulation.challengeMetrics.averageEffortRatio);
    hash.addDouble(simulation.challengeMetrics.averageCadenceRpm);
    hash.addDouble(simulation.challengeMetrics.averageSpeedKph);
    hash.addDouble(simulation.challengeMetrics.averageAdherence);
    hash.addDouble(simulation.challengeAssessment.readiness);
    hash.addDouble(simulation.challengeAssessment.effortReadiness);
    hash.addDouble(simulation.challengeAssessment.cadenceReadiness);
    hash.addDouble(simulation.challengeAssessment.speedReadiness);
    hash.addDouble(simulation.challengeAssessment.adherenceReadiness);
    hash.addBool(simulation.challengeAssessment.completed);
    hash.addBool(simulation.challengeMeasurementActive);
    hash.addDouble(simulation.challengeReadiness);
}

void addWorld(StateHasher &hash, const WorkoutGameWorldSnapshot &world)
{
    hash.addBool(world.ready);
    hash.addInteger(world.generation);
    hash.addEnum(world.terrain);
    hash.addInteger(world.seed);
    hash.addDouble(world.gradePercent);
    hash.addDouble(world.difficulty);
    hash.addDouble(world.terrainOffsetMeters);
    hash.addDouble(world.rider.distanceMeters);
    hash.addDouble(world.rider.elevationMeters);
    hash.addDouble(world.rider.pitchDegrees);
    hash.addDouble(world.rider.rollDegrees);
    hash.addDouble(world.rider.rearSuspension);
    hash.addDouble(world.rider.frontSuspension);
    hash.addDouble(world.rider.rearWheelRadians);
    hash.addDouble(world.rider.frontWheelRadians);
    hash.addDouble(world.rider.clearanceMeters);
    hash.addBool(world.rider.airborne);
    hash.addBool(world.rider.walking);
    hash.addDouble(world.speedMetersPerSecond);
    hash.addDouble(world.landingImpact);
}

void addCamera(StateHasher &hash, const WorkoutGameCameraSnapshot &camera)
{
    hash.addBool(camera.ready);
    hash.addEnum(camera.mode);
    hash.addDouble(camera.centerDistanceMeters);
    hash.addDouble(camera.centerElevationMeters);
    hash.addDouble(camera.lookAheadMeters);
    hash.addDouble(camera.zoom);
    hash.addDouble(camera.yawDegrees);
    hash.addDouble(camera.pitchDegrees);
}

void addFeature(
        StateHasher &hash,
        const WorkoutGameFeatureRuntimeSnapshot &feature)
{
    hash.addBool(feature.ready);
    hash.addInteger(feature.sourceSectionIndex);
    hash.addEnum(feature.terrain);
    hash.addEnum(feature.phase);
    hash.addEnum(feature.motion);
    hash.addEnum(feature.outcome);
    hash.addEnum(feature.route);
    hash.addDouble(feature.visualDistanceMeters);
    hash.addDouble(feature.prepareDistanceMeters);
    hash.addDouble(feature.decisionDistanceMeters);
    hash.addDouble(feature.obstacleDistanceMeters);
    hash.addDouble(feature.distanceToObstacleMeters);
    hash.addDouble(feature.readiness);
    hash.addDouble(feature.lateralOffsetMeters);
    hash.addDouble(feature.verticalOffsetMeters);
    hash.addDouble(feature.pitchDegrees);
    hash.addDouble(feature.vibration);
    hash.addDouble(feature.landingImpact);
    hash.addInteger(feature.actionId);
    hash.addBool(feature.triggerJump);
}

bool isFiniteValue(double value)
{
    return std::isfinite(value);
}

}

bool WorkoutGameReplayHarness::validInput(
        const WorkoutGameReplaySample &sample)
{
    const WorkoutGameSimulationInput &input = sample.input.simulation;
    return sample.presentationTimeMs >= 0
            && input.workoutTimeMs >= 0
            && isFiniteValue(input.actualWatts)
            && isFiniteValue(input.targetWatts)
            && isFiniteValue(input.cadenceRpm)
            && isFiniteValue(input.authoritativeSpeedKph)
            && isFiniteValue(input.drivetrainSpeedLimitKph);
}

bool WorkoutGameReplayHarness::finiteState(
        const WorkoutGameEngineFrame &frame)
{
    const WorkoutGameSimulationSnapshot &simulation = frame.visual.simulation;
    const WorkoutGameWorldSnapshot &world = frame.visual.world;
    const WorkoutGameCameraSnapshot &camera = frame.visual.camera;
    const WorkoutGameFeatureRuntimeSnapshot &feature = frame.visual.feature;
    return isFiniteValue(simulation.courseProgress)
            && isFiniteValue(simulation.sectionProgress)
            && isFiniteValue(simulation.speedKph)
            && isFiniteValue(simulation.adherence)
            && isFiniteValue(simulation.streakSeconds)
            && isFiniteValue(simulation.previousFeatureReadiness)
            && isFiniteValue(simulation.challengeMetrics.averageActualWatts)
            && isFiniteValue(simulation.challengeMetrics.averageTargetWatts)
            && isFiniteValue(simulation.challengeMetrics.averageEffortRatio)
            && isFiniteValue(simulation.challengeMetrics.averageCadenceRpm)
            && isFiniteValue(simulation.challengeMetrics.averageSpeedKph)
            && isFiniteValue(simulation.challengeMetrics.averageAdherence)
            && isFiniteValue(simulation.challengeAssessment.readiness)
            && isFiniteValue(simulation.challengeAssessment.effortReadiness)
            && isFiniteValue(simulation.challengeAssessment.cadenceReadiness)
            && isFiniteValue(simulation.challengeAssessment.speedReadiness)
            && isFiniteValue(simulation.challengeAssessment.adherenceReadiness)
            && isFiniteValue(simulation.challengeReadiness)
            && isFiniteValue(world.rider.distanceMeters)
            && isFiniteValue(world.rider.elevationMeters)
            && isFiniteValue(world.rider.pitchDegrees)
            && isFiniteValue(world.rider.rollDegrees)
            && isFiniteValue(world.rider.rearSuspension)
            && isFiniteValue(world.rider.frontSuspension)
            && isFiniteValue(world.rider.rearWheelRadians)
            && isFiniteValue(world.rider.frontWheelRadians)
            && isFiniteValue(world.rider.clearanceMeters)
            && isFiniteValue(world.speedMetersPerSecond)
            && isFiniteValue(world.landingImpact)
            && isFiniteValue(camera.centerDistanceMeters)
            && isFiniteValue(camera.centerElevationMeters)
            && isFiniteValue(camera.lookAheadMeters)
            && isFiniteValue(camera.zoom)
            && isFiniteValue(camera.yawDegrees)
            && isFiniteValue(camera.pitchDegrees)
            && isFiniteValue(feature.visualDistanceMeters)
            && isFiniteValue(feature.prepareDistanceMeters)
            && isFiniteValue(feature.decisionDistanceMeters)
            && isFiniteValue(feature.obstacleDistanceMeters)
            && isFiniteValue(feature.distanceToObstacleMeters)
            && isFiniteValue(feature.readiness)
            && isFiniteValue(feature.lateralOffsetMeters)
            && isFiniteValue(feature.verticalOffsetMeters)
            && isFiniteValue(feature.pitchDegrees)
            && isFiniteValue(feature.vibration)
            && isFiniteValue(feature.landingImpact)
            && isFiniteValue(frame.visual.riderPedalCycles)
            && isFiniteValue(frame.watts)
            && isFiniteValue(frame.targetWatts);
}

std::uint64_t WorkoutGameReplayHarness::hashFrame(
        std::uint64_t previousHash,
        const WorkoutGameReplaySample &sample,
        const WorkoutGameEngineFrame &frame)
{
    StateHasher hash(previousHash);
    hash.addInteger(WorkoutGameReplay::CurrentFormatVersion);
    hash.addInteger(sample.presentationTimeMs);
    hash.addInteger(sample.skippedTicks);
    hash.addInteger(sample.input.simulation.workoutTimeMs);
    hash.addDouble(sample.input.simulation.actualWatts);
    hash.addDouble(sample.input.simulation.targetWatts);
    hash.addDouble(sample.input.simulation.cadenceRpm);
    hash.addDouble(sample.input.simulation.authoritativeSpeedKph);
    hash.addDouble(sample.input.simulation.drivetrainSpeedLimitKph);
    hash.addInteger(sample.input.simulation.virtualGear);
    hash.addBool(sample.input.simulation.paused);
    hash.addInteger(sample.input.heartRate);
    hash.addInteger(sample.input.telemetryMonotonicTimeMs);
    addSimulation(hash, frame.visual.simulation);
    addWorld(hash, frame.visual.world);
    addCamera(hash, frame.visual.camera);
    addFeature(hash, frame.visual.feature);
    hash.addInteger(frame.visual.sessionGeneration);
    hash.addInteger(frame.visual.presentationTimeMs);
    hash.addInteger(frame.visual.skippedSimulationTicks);
    hash.addDouble(frame.visual.riderPedalCycles);
    hash.addDouble(frame.watts);
    hash.addDouble(frame.targetWatts);
    hash.addInteger(frame.cadenceRpm);
    hash.addInteger(frame.heartRate);
    hash.addInteger(frame.virtualGear);
    hash.addInteger(frame.sequence);
    hash.addInteger(frame.skippedTicks);
    hash.addBool(frame.telemetryStale);
    return hash.result();
}

WorkoutGameReplayResult WorkoutGameReplayHarness::run(
        const WorkoutGameReplay &replay)
{
    WorkoutGameReplayResult result;
    if (replay.formatVersion != WorkoutGameReplay::CurrentFormatVersion) {
        result.failure = WorkoutGameReplayFailure::UnsupportedFormat;
        return result;
    }
    if (replay.samples.empty()) {
        result.failure = WorkoutGameReplayFailure::EmptyReplay;
        return result;
    }

    WorkoutGameEngine engine;
    if (!isFiniteValue(replay.ftpWatts)
            || !engine.configure(
                replay.course, replay.ftpWatts, replay.featureLabEnabled)) {
        result.failure = WorkoutGameReplayFailure::InvalidConfiguration;
        return result;
    }

    result.frameStateHashes.reserve(replay.samples.size());
    std::uint64_t stateHash = HashOffset;
    std::int64_t previousPresentationTime = -1;
    std::int64_t previousWorkoutTime = -1;
    double previousCourseProgress = 0.0;
    double previousDistance = 0.0;
    for (std::size_t index = 0; index < replay.samples.size(); ++index) {
        const WorkoutGameReplaySample &sample = replay.samples[index];
        result.failureSample = index;
        if (!validInput(sample)) {
            result.failure = WorkoutGameReplayFailure::InvalidInput;
            return result;
        }
        if (sample.presentationTimeMs < previousPresentationTime) {
            result.failure = WorkoutGameReplayFailure::PresentationTimeRegression;
            return result;
        }
        if (sample.input.simulation.workoutTimeMs < previousWorkoutTime) {
            result.failure = WorkoutGameReplayFailure::WorkoutTimeRegression;
            return result;
        }

        const WorkoutGameEngineFrame frame = engine.update(
                sample.input,
                sample.presentationTimeMs,
                sample.skippedTicks);
        if (!finiteState(frame)) {
            result.failure = WorkoutGameReplayFailure::NonFiniteState;
            return result;
        }
        if (frame.visual.simulation.courseProgress + 1e-12
                    < previousCourseProgress
                || frame.visual.world.rider.distanceMeters + 1e-9
                    < previousDistance) {
            result.failure = WorkoutGameReplayFailure::CourseProgressRegression;
            return result;
        }

        stateHash = hashFrame(stateHash, sample, frame);
        result.frameStateHashes.push_back(stateHash);
        result.finalFrame = frame;
        previousPresentationTime = sample.presentationTimeMs;
        previousWorkoutTime = sample.input.simulation.workoutTimeMs;
        previousCourseProgress = frame.visual.simulation.courseProgress;
        previousDistance = frame.visual.world.rider.distanceMeters;
    }

    result.passed = true;
    result.failure = WorkoutGameReplayFailure::None;
    result.failureSample = replay.samples.size();
    result.finalStateHash = stateHash;
    return result;
}
