/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameVisualSmoother.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

double lerp(double from, double to, double amount)
{
    return from + (to - from) * amount;
}

double lerpAngle(double from, double to, double amount, double period)
{
    double difference = std::fmod(to - from, period);
    if (difference > period * 0.5) difference -= period;
    if (difference < -period * 0.5) difference += period;
    return from + difference * amount;
}

bool sameCompetitors(
        const WorkoutGameCompetitionSnapshot &from,
        const WorkoutGameCompetitionSnapshot &to)
{
    if (from.ready != to.ready
            || from.competitors.size() != to.competitors.size()) {
        return false;
    }
    for (std::size_t index = 0; index < from.competitors.size(); ++index) {
        const WorkoutGameCompetitorSnapshot &left = from.competitors[index];
        const WorkoutGameCompetitorSnapshot &right = to.competitors[index];
        if (left.kind != right.kind
                || left.identity != right.identity
                || left.lane != right.lane) {
            return false;
        }
    }
    return true;
}

double extrapolate(
        double previous,
        double target,
        std::int64_t predictionMs,
        std::int64_t sourceIntervalMs)
{
    return target + (target - previous)
            * double(predictionMs)
            / double(std::max<std::int64_t>(1, sourceIntervalMs));
}

}

void WorkoutGameVisualSmoother::reset()
{
    initialized = false;
    sourceAdvancing = false;
    fixedStepSnapshots = false;
    transitionStartMs = 0;
    lastTargetMonotonicMs = 0;
    sourceIntervalMs = 1000;
    previousPresentationTimeMs = 0;
    targetPresentationTimeMs = 0;
    previous = WorkoutGameVisualSnapshot();
    predictionOrigin = WorkoutGameVisualSnapshot();
    target = WorkoutGameVisualSnapshot();
    fixedStepHistory.clear();
    terrainTransition.reset();
}

void WorkoutGameVisualSmoother::setTarget(
        const WorkoutGameVisualSnapshot &snapshot,
        std::int64_t monotonicTimeMs)
{
    terrainTransition.setTarget(snapshot.world, monotonicTimeMs);
    const bool fixedStepTarget = snapshot.presentationTimeMs > 0;
    if (fixedStepTarget) {
        const std::int64_t presentationTimeMs = snapshot.presentationTimeMs;
        if (!initialized || !fixedStepSnapshots
                || presentationTimeMs <= targetPresentationTimeMs
                || isDiscontinuity(target, snapshot)) {
            initialized = true;
            fixedStepSnapshots = true;
            sourceAdvancing = false;
            previous = snapshot;
            target = snapshot;
            previousPresentationTimeMs = presentationTimeMs;
            targetPresentationTimeMs = presentationTimeMs;
            fixedStepHistory.clear();
            fixedStepHistory.push_back(snapshot);
            return;
        }
        previous = target;
        target = snapshot;
        previousPresentationTimeMs = targetPresentationTimeMs;
        targetPresentationTimeMs = presentationTimeMs;
        sourceAdvancing = target.simulation.workoutTimeMs
                >= previous.simulation.workoutTimeMs;
        fixedStepHistory.push_back(snapshot);
        while (fixedStepHistory.size() > 8) {
            fixedStepHistory.pop_front();
        }
        return;
    }

    if (!initialized
            || monotonicTimeMs < transitionStartMs
            || isDiscontinuity(target, snapshot)) {
        initialized = true;
        fixedStepSnapshots = false;
        sourceAdvancing = false;
        transitionStartMs = monotonicTimeMs;
        lastTargetMonotonicMs = monotonicTimeMs;
        sourceIntervalMs = 1000;
        previous = snapshot;
        predictionOrigin = snapshot;
        target = snapshot;
        return;
    }

    const WorkoutGameVisualSnapshot sampled = sample(monotonicTimeMs);
    const bool advancing = snapshot.simulation.workoutTimeMs
            > target.simulation.workoutTimeMs;
    const std::int64_t workoutIntervalMs =
            snapshot.simulation.workoutTimeMs
            - target.simulation.workoutTimeMs;
    const std::int64_t monotonicIntervalMs =
            monotonicTimeMs - lastTargetMonotonicMs;
    const std::int64_t observedIntervalMs = workoutIntervalMs > 0
            ? workoutIntervalMs
            : monotonicIntervalMs;
    if (observedIntervalMs > 0) {
        sourceIntervalMs = std::clamp(
                observedIntervalMs,
                MinimumSourceIntervalMs,
                MaximumSourceIntervalMs);
    }
    previous = sampled;
    predictionOrigin = target;
    target = snapshot;
    transitionStartMs = monotonicTimeMs;
    lastTargetMonotonicMs = monotonicTimeMs;
    sourceAdvancing = advancing;

    if (sourceAdvancing
            && target.simulation.workoutTimeMs
                < sampled.simulation.workoutTimeMs) {
        target.simulation.workoutTimeMs =
                sampled.simulation.workoutTimeMs;
        target.simulation.courseProgress = std::max(
                target.simulation.courseProgress,
                sampled.simulation.courseProgress);
        if (target.simulation.activeSection
                == sampled.simulation.activeSection) {
            target.simulation.sectionProgress = std::max(
                    target.simulation.sectionProgress,
                    sampled.simulation.sectionProgress);
        }
        if (target.world.ready && sampled.world.ready
                && target.world.generation == sampled.world.generation) {
            target.world.rider.distanceMeters = std::max(
                    target.world.rider.distanceMeters,
                    sampled.world.rider.distanceMeters);
        }
        if (target.camera.ready && sampled.camera.ready) {
            target.camera.centerDistanceMeters = std::max(
                    target.camera.centerDistanceMeters,
                    sampled.camera.centerDistanceMeters);
        }
        predictionOrigin = target;
        transitionStartMs -= TransitionDurationMs;
    }
}

WorkoutGameVisualSnapshot WorkoutGameVisualSmoother::sample(
        std::int64_t monotonicTimeMs) const
{
    if (!initialized) return WorkoutGameVisualSnapshot();
    if (fixedStepSnapshots) {
        if (fixedStepHistory.empty()) return target;
        const std::int64_t renderTimeMs = monotonicTimeMs
                - FixedStepPresentationDelayMs;
        std::size_t upperIndex = 0;
        while (upperIndex < fixedStepHistory.size()
                && fixedStepHistory[upperIndex].presentationTimeMs
                    <= renderTimeMs) {
            ++upperIndex;
        }
        WorkoutGameVisualSnapshot result;
        if (upperIndex == 0) {
            result = fixedStepHistory.front();
        } else if (upperIndex >= fixedStepHistory.size()) {
            result = fixedStepHistory.back();
        } else {
            const WorkoutGameVisualSnapshot &from =
                    fixedStepHistory[upperIndex - 1];
            const WorkoutGameVisualSnapshot &to =
                    fixedStepHistory[upperIndex];
            const std::int64_t intervalMs = to.presentationTimeMs
                    - from.presentationTimeMs;
            const double amount = intervalMs > 0
                    ? std::clamp(
                        double(renderTimeMs - from.presentationTimeMs)
                            / double(intervalMs),
                        0.0, 1.0)
                    : 1.0;
            result = interpolate(from, to, amount);
        }
        result.terrainTransition = terrainTransition.sample(monotonicTimeMs);
        return result;
    }
    const std::int64_t elapsedMs = std::max<std::int64_t>(
            0, monotonicTimeMs - transitionStartMs);
    const double amount = std::clamp(
            double(elapsedMs) / double(TransitionDurationMs),
            0.0, 1.0);
    WorkoutGameVisualSnapshot result = interpolate(previous, target, amount);
    result.terrainTransition = terrainTransition.sample(monotonicTimeMs);

    const std::int64_t predictionMs = std::clamp<std::int64_t>(
            elapsedMs - TransitionDurationMs, 0, MaximumPredictionMs);
    const bool movingForward = sourceAdvancing
            && target.simulation.ready
            && !target.simulation.finished
            && target.simulation.workoutTimeMs >= 0;
    if (predictionMs <= 0 || !movingForward) return result;

    result.simulation.workoutTimeMs += predictionMs;
    result.simulation.courseProgress = std::clamp(extrapolate(
            predictionOrigin.simulation.courseProgress,
            target.simulation.courseProgress,
            predictionMs, sourceIntervalMs), 0.0, 1.0);
    result.simulation.sectionProgress = std::clamp(extrapolate(
            predictionOrigin.simulation.sectionProgress,
            target.simulation.sectionProgress,
            predictionMs, sourceIntervalMs), 0.0, 1.0);
    if (result.world.ready) {
        const double predictionSeconds = double(predictionMs) / 1000.0;
        result.world.rider.distanceMeters +=
                result.world.speedMetersPerSecond * predictionSeconds;
        result.world.rider.elevationMeters = extrapolate(
                predictionOrigin.world.rider.elevationMeters,
                target.world.rider.elevationMeters,
                predictionMs, sourceIntervalMs);
    }
    if (result.camera.ready) {
        const double predictionSeconds = double(predictionMs) / 1000.0;
        result.camera.centerDistanceMeters +=
                result.world.speedMetersPerSecond * predictionSeconds;
        result.camera.centerElevationMeters = extrapolate(
                predictionOrigin.camera.centerElevationMeters,
                target.camera.centerElevationMeters,
                predictionMs, sourceIntervalMs);
    }
    for (std::size_t index = 0;
         index < result.competition.competitors.size(); ++index) {
        const WorkoutGameCompetitorSnapshot &origin =
                predictionOrigin.competition.competitors[index];
        result.competition.competitors[index].courseProgress = std::clamp(
                extrapolate(
                    origin.courseProgress,
                    target.competition.competitors[index].courseProgress,
                    predictionMs, sourceIntervalMs),
                0.0, 1.0);
        result.competition.competitors[index].relativeProgress = extrapolate(
                origin.relativeProgress,
                target.competition.competitors[index].relativeProgress,
                predictionMs, sourceIntervalMs);
    }
    return result;
}

bool WorkoutGameVisualSmoother::isDiscontinuity(
        const WorkoutGameVisualSnapshot &from,
        const WorkoutGameVisualSnapshot &to)
{
    const bool worldReset = from.world.ready && to.world.ready
            && from.world.generation != to.world.generation
            && to.world.rider.distanceMeters + 2.0
                    < from.world.rider.distanceMeters;
    return from.simulation.ready != to.simulation.ready
            || from.simulation.finished != to.simulation.finished
            || from.world.ready != to.world.ready
            || worldReset
            || from.camera.ready != to.camera.ready
            || !sameCompetitors(from.competition, to.competition);
}

WorkoutGameVisualSnapshot WorkoutGameVisualSmoother::interpolate(
        const WorkoutGameVisualSnapshot &from,
        const WorkoutGameVisualSnapshot &to,
        double amount)
{
    WorkoutGameVisualSnapshot result = to;

    result.simulation.workoutTimeMs = std::llround(lerp(
            double(from.simulation.workoutTimeMs),
            double(to.simulation.workoutTimeMs), amount));
    result.simulation.courseProgress = lerp(
            from.simulation.courseProgress, to.simulation.courseProgress, amount);
    result.simulation.sectionProgress = lerp(
            from.simulation.sectionProgress, to.simulation.sectionProgress, amount);
    result.simulation.speedKph = lerp(
            from.simulation.speedKph, to.simulation.speedKph, amount);
    result.simulation.adherence = lerp(
            from.simulation.adherence, to.simulation.adherence, amount);
    result.simulation.streakSeconds = lerp(
            from.simulation.streakSeconds, to.simulation.streakSeconds, amount);
    result.simulation.challengeReadiness = lerp(
            from.simulation.challengeReadiness,
            to.simulation.challengeReadiness,
            amount);
    result.riderPedalCycles = lerp(
            from.riderPedalCycles, to.riderPedalCycles, amount);
    for (std::size_t index = 0;
         index < result.competition.competitors.size(); ++index) {
        result.competition.competitors[index].courseProgress = lerp(
                from.competition.competitors[index].courseProgress,
                to.competition.competitors[index].courseProgress, amount);
        result.competition.competitors[index].relativeProgress = lerp(
                from.competition.competitors[index].relativeProgress,
                to.competition.competitors[index].relativeProgress, amount);
    }

    result.world.gradePercent = lerp(
            from.world.gradePercent, to.world.gradePercent, amount);
    result.world.difficulty = lerp(
            from.world.difficulty, to.world.difficulty, amount);
    result.world.terrainOffsetMeters = from.world.generation
                    == to.world.generation
            ? lerp(from.world.terrainOffsetMeters,
                   to.world.terrainOffsetMeters, amount)
            : to.world.terrainOffsetMeters;
    result.world.speedMetersPerSecond = lerp(
            from.world.speedMetersPerSecond,
            to.world.speedMetersPerSecond, amount);
    result.world.landingImpact = lerp(
            from.world.landingImpact, to.world.landingImpact, amount);
    result.world.rider.distanceMeters = lerp(
            from.world.rider.distanceMeters,
            to.world.rider.distanceMeters, amount);
    result.world.rider.elevationMeters = lerp(
            from.world.rider.elevationMeters,
            to.world.rider.elevationMeters, amount);
    result.world.rider.pitchDegrees = lerpAngle(
            from.world.rider.pitchDegrees,
            to.world.rider.pitchDegrees, amount, 360.0);
    result.world.rider.rollDegrees = lerpAngle(
            from.world.rider.rollDegrees,
            to.world.rider.rollDegrees, amount, 360.0);
    result.world.rider.rearSuspension = lerp(
            from.world.rider.rearSuspension,
            to.world.rider.rearSuspension, amount);
    result.world.rider.frontSuspension = lerp(
            from.world.rider.frontSuspension,
            to.world.rider.frontSuspension, amount);
    result.world.rider.rearWheelRadians = lerpAngle(
            from.world.rider.rearWheelRadians,
            to.world.rider.rearWheelRadians, amount, 2.0 * std::acos(-1.0));
    result.world.rider.frontWheelRadians = lerpAngle(
            from.world.rider.frontWheelRadians,
            to.world.rider.frontWheelRadians, amount, 2.0 * std::acos(-1.0));
    result.world.rider.clearanceMeters = lerp(
            from.world.rider.clearanceMeters,
            to.world.rider.clearanceMeters, amount);

    if (from.feature.ready && to.feature.ready
            && from.feature.sourceSectionIndex
                == to.feature.sourceSectionIndex
            && from.feature.actionId == to.feature.actionId) {
        result.feature.visualDistanceMeters = lerp(
                from.feature.visualDistanceMeters,
                to.feature.visualDistanceMeters, amount);
        result.feature.distanceToObstacleMeters = lerp(
                from.feature.distanceToObstacleMeters,
                to.feature.distanceToObstacleMeters, amount);
        result.feature.readiness = lerp(
                from.feature.readiness, to.feature.readiness, amount);
        result.feature.lateralOffset = lerp(
                from.feature.lateralOffset,
                to.feature.lateralOffset, amount);
        result.feature.verticalOffsetMeters = lerp(
                from.feature.verticalOffsetMeters,
                to.feature.verticalOffsetMeters, amount);
        result.feature.pitchDegrees = lerp(
                from.feature.pitchDegrees,
                to.feature.pitchDegrees, amount);
        result.feature.vibration = lerp(
                from.feature.vibration, to.feature.vibration, amount);
        result.feature.landingImpact = lerp(
                from.feature.landingImpact,
                to.feature.landingImpact, amount);
    }

    result.camera.centerDistanceMeters = lerp(
            from.camera.centerDistanceMeters,
            to.camera.centerDistanceMeters, amount);
    result.camera.centerElevationMeters = lerp(
            from.camera.centerElevationMeters,
            to.camera.centerElevationMeters, amount);
    result.camera.lookAheadMeters = lerp(
            from.camera.lookAheadMeters, to.camera.lookAheadMeters, amount);
    result.camera.zoom = lerp(from.camera.zoom, to.camera.zoom, amount);
    result.camera.yawDegrees = lerpAngle(
            from.camera.yawDegrees, to.camera.yawDegrees, amount, 360.0);
    result.camera.pitchDegrees = lerpAngle(
            from.camera.pitchDegrees, to.camera.pitchDegrees, amount, 360.0);
    return result;
}

void WorkoutGameFrameRateCounter::reset()
{
    initialized = false;
    lastFrameNs = 0;
    intervalTotalNs = 0;
    recentIntervalsNs.clear();
    currentFps = 0.0;
    p95FrameIntervalMs = 0.0;
}

double WorkoutGameFrameRateCounter::frameRendered(
        std::int64_t monotonicTimeMs)
{
    return frameRenderedNanoseconds(monotonicTimeMs * 1000000);
}

double WorkoutGameFrameRateCounter::frameRenderedNanoseconds(
        std::int64_t monotonicTimeNs)
{
    if (!initialized || monotonicTimeNs < lastFrameNs) {
        initialized = true;
        lastFrameNs = monotonicTimeNs;
        intervalTotalNs = 0;
        recentIntervalsNs.clear();
        currentFps = 0.0;
        p95FrameIntervalMs = 0.0;
        return currentFps;
    }

    const std::int64_t intervalNs = monotonicTimeNs - lastFrameNs;
    lastFrameNs = monotonicTimeNs;
    if (intervalNs <= 0) return currentFps;
    recentIntervalsNs.push_back(intervalNs);
    intervalTotalNs += intervalNs;
    while (recentIntervalsNs.size() > 2
            && intervalTotalNs > 1000000000ll) {
        intervalTotalNs -= recentIntervalsNs.front();
        recentIntervalsNs.pop_front();
    }
    currentFps = double(recentIntervalsNs.size()) * 1000000000.0
            / double(std::max<std::int64_t>(1, intervalTotalNs));
    std::vector<std::int64_t> ordered(
            recentIntervalsNs.begin(), recentIntervalsNs.end());
    std::sort(ordered.begin(), ordered.end());
    const std::size_t percentileIndex = ordered.empty() ? 0
            : std::min(
                ordered.size() - 1,
                std::size_t(std::ceil(ordered.size() * 0.95)) - 1);
    p95FrameIntervalMs = ordered.empty()
            ? 0.0 : double(ordered[percentileIndex]) / 1000000.0;
    return currentFps;
}
