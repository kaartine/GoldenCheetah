/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutRideCommandFilter.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double MaximumTargetWatts = 1500.0;
constexpr double MaxSlewWattsPerSecond = 100.0;
constexpr std::int64_t MinimumDispatchIntervalMs = 250;

double normalizedWatts(double watts)
{
    return std::round(std::clamp(watts, 0.0, MaximumTargetWatts));
}

}

WorkoutRideCommandDecision WorkoutRideCommandFilter::update(
        double requestedWatts,
        double baseWorkoutWatts,
        std::int64_t nowMs)
{
    WorkoutRideCommandDecision result;
    if (initialized) {
        result.hasEffectiveTarget = true;
        result.effectiveWatts = lastSentWatts;
    }

    if (!std::isfinite(requestedWatts)
            || !std::isfinite(baseWorkoutWatts)
            || requestedWatts < 0.0
            || baseWorkoutWatts < 0.0) {
        return result;
    }

    const double requested = normalizedWatts(requestedWatts);
    const double baseWorkout = normalizedWatts(baseWorkoutWatts);
    if (!initialized || nowMs < lastDispatchMs) {
        initialized = true;
        lastSentWatts = requested;
        lastBaseWorkoutWatts = baseWorkout;
        lastDispatchMs = nowMs;
        return {true, true, lastSentWatts, -1};
    }

    const bool workoutStepChanged = baseWorkout != lastBaseWorkoutWatts;
    lastBaseWorkoutWatts = baseWorkout;
    if (workoutStepChanged) {
        lastSentWatts = requested;
        lastDispatchMs = nowMs;
        return {true, true, lastSentWatts, -1};
    }

    if (requested == lastSentWatts) return result;

    const std::int64_t elapsedMs = nowMs - lastDispatchMs;
    if (elapsedMs < MinimumDispatchIntervalMs) {
        result.retryAfterMs = int(MinimumDispatchIntervalMs - elapsedMs);
        return result;
    }

    const double allowedChange = MaxSlewWattsPerSecond
            * double(elapsedMs) / 1000.0;
    const double limited = std::clamp(
            requested,
            lastSentWatts - allowedChange,
            lastSentWatts + allowedChange);
    const double command = normalizedWatts(limited);
    if (command == lastSentWatts) {
        result.retryAfterMs = int(MinimumDispatchIntervalMs);
        return result;
    }

    lastSentWatts = command;
    lastDispatchMs = nowMs;
    result.dispatch = true;
    result.effectiveWatts = lastSentWatts;
    if (lastSentWatts != requested) {
        result.retryAfterMs = int(MinimumDispatchIntervalMs);
    }
    return result;
}

void WorkoutRideCommandFilter::reset()
{
    initialized = false;
    lastSentWatts = 0.0;
    lastBaseWorkoutWatts = 0.0;
    lastDispatchMs = 0;
}
