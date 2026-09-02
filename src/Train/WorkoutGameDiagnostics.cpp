/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameDiagnostics.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double MovementEpsilonMeters = 1e-6;
constexpr double VisibleAirborneHeightMeters = 0.08;
constexpr std::int64_t LateFrameThresholdMs = 25;
constexpr double LongPresentationWorkThresholdMs = 8.0;

}

void WorkoutGameDiagnostics::reset()
{
    initialized = false;
    frameTimingInitialized = false;
    previousRoadDistanceMeters = 0.0;
    previousMonotonicTimeMs = 0;
    previousRenderedWorkoutTimeMs = 0;
    backwardFrameCount = 0;
    stationaryFrameCount = 0;
    lateFrameCount = 0;
    longPresentationWorkCount = 0;
    unexpectedAirborneFrameCount = 0;
    largestFrameIntervalMs = 0;
    largestPresentationWorkMs = 0.0;
    largestRegressionMeters = 0.0;
}

WorkoutGameDiagnosticsSnapshot WorkoutGameDiagnostics::update(
        const WorkoutGameDiagnosticsInput &input)
{
    WorkoutGameDiagnosticsSnapshot result;
    result.input = input;
    if (!input.ready || !input.sessionRunning) {
        reset();
        return result;
    }

    const bool timelineReset = initialized
            && input.renderedWorkoutTimeMs < previousRenderedWorkoutTimeMs;
    if (timelineReset) reset();
    if (!initialized) {
        initialized = true;
        previousRoadDistanceMeters = input.renderedRoadDistanceMeters;
        previousMonotonicTimeMs = input.monotonicTimeMs;
        previousRenderedWorkoutTimeMs = input.renderedWorkoutTimeMs;
        frameTimingInitialized = input.frameTimingWarmupComplete;
        result.ready = true;
        result.backwardFrameCount = backwardFrameCount;
        result.stationaryFrameCount = stationaryFrameCount;
        result.lateFrameCount = lateFrameCount;
        result.longPresentationWorkCount = longPresentationWorkCount;
        result.unexpectedAirborneFrameCount = unexpectedAirborneFrameCount;
        result.largestFrameIntervalMs = largestFrameIntervalMs;
        result.largestPresentationWorkMs = largestPresentationWorkMs;
        result.largestRegressionMeters = largestRegressionMeters;
        return result;
    }

    result.ready = true;
    if (!input.frameTimingWarmupComplete) {
        frameTimingInitialized = false;
    } else if (!frameTimingInitialized) {
        frameTimingInitialized = true;
    } else {
        result.frameIntervalMs = std::max<std::int64_t>(
                0, input.monotonicTimeMs - previousMonotonicTimeMs);
        largestFrameIntervalMs = std::max(
                largestFrameIntervalMs, result.frameIntervalMs);
        if (result.frameIntervalMs > LateFrameThresholdMs) ++lateFrameCount;
    }
    const double presentationWorkMs =
            std::isfinite(input.presentationWorkMs)
            ? std::max(0.0, input.presentationWorkMs) : 0.0;
    largestPresentationWorkMs = std::max(
            largestPresentationWorkMs, presentationWorkMs);
    if (presentationWorkMs > LongPresentationWorkThresholdMs) {
        ++longPresentationWorkCount;
    }
    if (input.worldReady && input.riderAirborne
            && input.airHeightMeters >= VisibleAirborneHeightMeters
            && !input.airborneExpected) {
        ++unexpectedAirborneFrameCount;
    }
    result.frameDistanceDeltaMeters = input.renderedRoadDistanceMeters
            - previousRoadDistanceMeters;
    if (input.movingForward
            && result.frameDistanceDeltaMeters < -MovementEpsilonMeters) {
        ++backwardFrameCount;
        largestRegressionMeters = std::max(
                largestRegressionMeters,
                -result.frameDistanceDeltaMeters);
    } else if (input.movingForward
            && std::abs(result.frameDistanceDeltaMeters)
                <= MovementEpsilonMeters) {
        ++stationaryFrameCount;
    }
    result.backwardFrameCount = backwardFrameCount;
    result.stationaryFrameCount = stationaryFrameCount;
    result.lateFrameCount = lateFrameCount;
    result.longPresentationWorkCount = longPresentationWorkCount;
    result.unexpectedAirborneFrameCount = unexpectedAirborneFrameCount;
    result.largestFrameIntervalMs = largestFrameIntervalMs;
    result.largestPresentationWorkMs = largestPresentationWorkMs;
    result.largestRegressionMeters = largestRegressionMeters;
    previousRoadDistanceMeters = input.renderedRoadDistanceMeters;
    previousMonotonicTimeMs = input.monotonicTimeMs;
    previousRenderedWorkoutTimeMs = input.renderedWorkoutTimeMs;
    return result;
}
