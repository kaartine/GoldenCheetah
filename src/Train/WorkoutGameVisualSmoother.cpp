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

}

void WorkoutGameVisualSmoother::reset()
{
    initialized = false;
    transitionStartMs = 0;
    previous = WorkoutGameVisualSnapshot();
    target = WorkoutGameVisualSnapshot();
}

void WorkoutGameVisualSmoother::setTarget(
        const WorkoutGameVisualSnapshot &snapshot,
        std::int64_t monotonicTimeMs)
{
    if (!initialized
            || monotonicTimeMs < transitionStartMs
            || isDiscontinuity(target, snapshot)) {
        initialized = true;
        transitionStartMs = monotonicTimeMs;
        previous = snapshot;
        target = snapshot;
        return;
    }

    previous = sample(monotonicTimeMs);
    target = snapshot;
    transitionStartMs = monotonicTimeMs;
}

WorkoutGameVisualSnapshot WorkoutGameVisualSmoother::sample(
        std::int64_t monotonicTimeMs) const
{
    if (!initialized) return WorkoutGameVisualSnapshot();
    const double amount = std::clamp(
            double(monotonicTimeMs - transitionStartMs)
                    / double(TransitionDurationMs),
            0.0, 1.0);
    return interpolate(previous, target, amount);
}

bool WorkoutGameVisualSmoother::isDiscontinuity(
        const WorkoutGameVisualSnapshot &from,
        const WorkoutGameVisualSnapshot &to)
{
    return from.simulation.ready != to.simulation.ready
            || from.simulation.finished != to.simulation.finished
            || from.simulation.activeSection != to.simulation.activeSection
            || from.world.ready != to.world.ready
            || (from.world.ready
                && (from.world.generation != to.world.generation
                    || from.world.terrain != to.world.terrain))
            || from.camera.ready != to.camera.ready;
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

    result.world.gradePercent = lerp(
            from.world.gradePercent, to.world.gradePercent, amount);
    result.world.difficulty = lerp(
            from.world.difficulty, to.world.difficulty, amount);
    result.world.terrainOffsetMeters = lerp(
            from.world.terrainOffsetMeters, to.world.terrainOffsetMeters, amount);
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
    windowStartMs = 0;
    frameIntervals = 0;
    currentFps = 0.0;
}

double WorkoutGameFrameRateCounter::frameRendered(
        std::int64_t monotonicTimeMs)
{
    if (!initialized || monotonicTimeMs < windowStartMs) {
        initialized = true;
        windowStartMs = monotonicTimeMs;
        frameIntervals = 0;
        currentFps = 0.0;
        return currentFps;
    }

    ++frameIntervals;
    const std::int64_t elapsedMs = monotonicTimeMs - windowStartMs;
    if (elapsedMs >= 1000) {
        currentFps = double(frameIntervals) * 1000.0 / double(elapsedMs);
        windowStartMs = monotonicTimeMs;
        frameIntervals = 0;
    }
    return currentFps;
}
