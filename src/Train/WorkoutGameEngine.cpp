/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameEngine.h"

#include "WorkoutGameRoadCourse.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double MaximumPowerWatts = 10000.0;
constexpr double MaximumCadenceRpm = 300.0;
constexpr double MaximumSpeedKph = 300.0;

double finiteClampedNonNegative(double value, double maximum)
{
    return std::isfinite(value)
            ? std::clamp(value, 0.0, maximum) : 0.0;
}

double finiteOptionalNonNegative(double value, double maximum)
{
    return std::isfinite(value)
            ? std::clamp(value, -1.0, maximum) : -1.0;
}

}

bool WorkoutGameEngine::configure(
        const WorkoutGameCourse &newCourse,
        double newFtpWatts,
        bool newFeatureLabEnabled)
{
    course = newCourse;
    ftpWatts = newFtpWatts;
    featureLabEnabled = newFeatureLabEnabled;
    configured = simulation.configure(course, ftpWatts);
    roadCourse = WorkoutGameRoadCourseBuilder::build(course, ftpWatts);
    if (!physics.configure(roadCourse)) {
        physics.configure(course.seed);
    }
    reset();
    return configured;
}

void WorkoutGameEngine::reset()
{
    simulation.reset();
    physics.reset();
    camera.reset();
    if (configured) {
        featureRuntime.configure(roadCourse);
    } else {
        featureRuntime.reset();
    }
    audioEventJournal.reset();
    worldClockInitialized = false;
    lastWorldTimeMs = 0;
    riderPedalCycles = 0.0;
    sequence = 0;
}

WorkoutGameEngineFrame WorkoutGameEngine::update(
        const WorkoutGameEngineInput &requestedInput,
        std::int64_t presentationTimeMs,
        std::size_t skippedTicks)
{
    WorkoutGameEngineFrame result;
    if (!configured) return result;

    WorkoutGameSimulationInput input = requestedInput.simulation;
    input.actualWatts = finiteClampedNonNegative(
            input.actualWatts, MaximumPowerWatts);
    input.targetWatts = finiteClampedNonNegative(
            input.targetWatts, MaximumPowerWatts);
    input.cadenceRpm = finiteClampedNonNegative(
            input.cadenceRpm, MaximumCadenceRpm);
    input.authoritativeSpeedKph = finiteOptionalNonNegative(
            input.authoritativeSpeedKph, MaximumSpeedKph);
    input.drivetrainSpeedLimitKph = finiteOptionalNonNegative(
            input.drivetrainSpeedLimitKph, MaximumSpeedKph);
    WorkoutGameSimulationSnapshot snapshot = simulation.update(input);
    double featureTargetWatts = input.targetWatts;
    if (featureTargetWatts <= 0.0
            && snapshot.activeSection >= 0
            && snapshot.activeSection < int(course.sections.size())) {
        featureTargetWatts = std::max(
                0.0,
                course.sections[std::size_t(snapshot.activeSection)]
                    .targetWatts);
    }
    const WorkoutGameFeatureRuntimeSnapshot feature =
            featureRuntime.update(snapshot, input.actualWatts,
                                  featureTargetWatts);
    if (feature.ready
            && feature.terrain == WorkoutGameTerrainKind::GapJump
            && feature.gapLineLocked
            && (feature.outcome == WorkoutGameFeatureOutcome::Completed
                || feature.outcome == WorkoutGameFeatureOutcome::Bypassed)) {
        simulation.commitGapJumpOutcome(
                snapshot.activeSection, feature.outcome, feature.readiness);
        snapshot.featureOutcome = feature.outcome;
        snapshot.route = feature.route;
        snapshot.challengeReadiness = feature.readiness;
        snapshot.challengeAssessment.readiness = feature.readiness;
        snapshot.challengeAssessment.completed = feature.outcome
                == WorkoutGameFeatureOutcome::Completed;
        snapshot.score = simulation.currentScore();
    }
    WorkoutGameWorldSnapshot world;
    WorkoutGameCameraSnapshot view;
    if (snapshot.ready
            && snapshot.activeSection >= 0
            && snapshot.activeSection < int(course.sections.size())) {
        const WorkoutGameSection &section =
                course.sections[std::size_t(snapshot.activeSection)];
        const double target = std::max(
                1.0,
                input.targetWatts > 0.0
                        ? input.targetWatts : section.targetWatts);
        WorkoutGamePhysicsInput physicsInput;
        physicsInput.workoutTimeMs = snapshot.workoutTimeMs;
        const WorkoutGameRoadTimelineSample road =
                WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(
                    roadCourse, snapshot.workoutTimeMs);
        physicsInput.courseDistanceMeters = road.ready
                ? road.distanceMeters : -1.0;
        if (snapshot.activeSection
                < int(roadCourse.timeline.size())) {
            const WorkoutGameRoadTimelineSection &timeline =
                    roadCourse.timeline[std::size_t(snapshot.activeSection)];
            if (timeline.durationMs > 0) {
                physicsInput.courseSpeedMetersPerSecond =
                        (timeline.endDistanceMeters
                         - timeline.startDistanceMeters)
                        * 1000.0 / double(timeline.durationMs);
            }
        }
        physicsInput.terrain = section.terrain;
        physicsInput.desiredSpeedMetersPerSecond = snapshot.speedKph / 3.6;
        const WorkoutGameRoadSample roadSurface = road.ready
                ? WorkoutGameRoadCourseBuilder::sample(
                    roadCourse, road.distanceMeters)
                : WorkoutGameRoadSample();
        physicsInput.gradePercent = roadSurface.ready
                ? roadSurface.center.gradePercent : section.gradePercent;
        physicsInput.difficulty = section.difficulty;
        physicsInput.effortRatio = std::max(0.0, input.actualWatts) / target;
        physicsInput.paused = input.paused;
        physicsInput.jumpRequested = feature.triggerJump;
        physicsInput.gapJumpLine = feature.lockedGapLine;
        physicsInput.gapJumpLaunchSpeedMetersPerSecond =
                feature.launchBestSpeedMetersPerSecond > 0.0
                ? feature.launchBestSpeedMetersPerSecond : -1.0;
        if (feature.terrain == WorkoutGameTerrainKind::GapJump
                && feature.lockedGapLine
                    != WorkoutGameGapJumpLine::None) {
            physicsInput.gapJumpTakeoffDistanceMeters =
                    feature.physicalTakeoffDistanceMeters;
            physicsInput.gapJumpLandingDistanceMeters =
                    feature.actionEndDistanceMeters;
        }
        physicsInput.forceGroundFollowing = feature.route
                == WorkoutGameRoute::SafeBypass;
        physicsInput.followCourseSurface =
                (feature.motion == WorkoutGameFeatureMotion::Jump
                 || feature.motion == WorkoutGameFeatureMotion::Drop)
                && !WorkoutGameFeatureRuntime::airborneExpected(feature);
        physicsInput.featureActionId = feature.actionId;
        world = physics.update(physicsInput);

        const double elapsedSeconds = worldClockInitialized
                && snapshot.workoutTimeMs >= lastWorldTimeMs
                && !input.paused
                ? double(snapshot.workoutTimeMs - lastWorldTimeMs) / 1000.0
                : 0.0;
        riderPedalCycles += elapsedSeconds
                * std::max(0.0, input.cadenceRpm) / 60.0;
        view = camera.update(world, elapsedSeconds);
        worldClockInitialized = true;
        lastWorldTimeMs = snapshot.workoutTimeMs;
    } else {
        camera.reset();
        worldClockInitialized = false;
        lastWorldTimeMs = input.workoutTimeMs;
    }

    result.visual = {snapshot, {}, world, view, {}, feature};
    result.visual.presentationTimeMs = presentationTimeMs;
    result.visual.skippedSimulationTicks = skippedTicks;
    result.visual.riderPedalCycles = riderPedalCycles;
    result.audioEvents = audioEventJournal.update(
            feature, world, snapshot.workoutTimeMs);
    result.watts = std::max(0.0, input.actualWatts);
    result.targetWatts = std::max(0.0, input.targetWatts);
    result.cadenceRpm = std::max(0, int(std::lround(input.cadenceRpm)));
    result.heartRate = std::clamp(requestedInput.heartRate, 0, 300);
    result.virtualGear = std::max(1, input.virtualGear);
    result.sequence = ++sequence;
    result.skippedTicks = skippedTicks;
    return result;
}

void WorkoutGameEngine::resynchronize(
        const WorkoutGameEngineInput &requestedInput,
        std::int64_t presentationTimeMs,
        std::size_t skippedTicks)
{
    WorkoutGameEngineInput input = requestedInput;
    input.simulation.paused = true;
    update(input, presentationTimeMs, skippedTicks);
}
