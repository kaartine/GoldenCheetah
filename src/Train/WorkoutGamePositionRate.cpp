/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGamePositionRate.h"

#include <algorithm>
#include <cmath>

namespace {

double validRate(double rate)
{
    return std::clamp(std::isfinite(rate) ? rate : 1.0, 0.0, 4.0);
}

}

void WorkoutGamePositionRate::reset(double initialRate)
{
    initialized = false;
    lastChangedWorkoutTimeMs = 0;
    lastChangedMonotonicTimeMs = 0;
    currentRate = validRate(initialRate);
    lastMovingRate = currentRate > 0.0 ? currentRate : 1.0;
}

double WorkoutGamePositionRate::update(
        const WorkoutGamePositionRateInput &input)
{
    if (!initialized
            || input.monotonicTimeMs < lastChangedMonotonicTimeMs
            || input.workoutTimeMs < lastChangedWorkoutTimeMs) {
        initialized = true;
        lastChangedWorkoutTimeMs = std::max<std::int64_t>(
                0, input.workoutTimeMs);
        lastChangedMonotonicTimeMs = input.monotonicTimeMs;
        if (input.motionKnown && !input.moving) currentRate = 0.0;
        return currentRate;
    }

    if (input.motionKnown && !input.moving) {
        if (currentRate > 0.0) lastMovingRate = currentRate;
        currentRate = 0.0;
        lastChangedWorkoutTimeMs = input.workoutTimeMs;
        lastChangedMonotonicTimeMs = input.monotonicTimeMs;
        return currentRate;
    }

    if (input.motionKnown && input.moving && currentRate <= 0.0) {
        currentRate = std::max(0.05, lastMovingRate);
    }

    if (input.workoutTimeMs == lastChangedWorkoutTimeMs) {
        return currentRate;
    }

    const std::int64_t elapsedMs = input.monotonicTimeMs
            - lastChangedMonotonicTimeMs;
    const std::int64_t workoutDeltaMs = input.workoutTimeMs
            - lastChangedWorkoutTimeMs;
    if (elapsedMs > 0 && workoutDeltaMs > 0) {
        const double measured = validRate(
                double(workoutDeltaMs) / double(elapsedMs));
        const double blend = std::clamp(
                1.0 - std::exp(-double(elapsedMs) / 600.0), 0.15, 0.8);
        currentRate += (measured - currentRate) * blend;
        currentRate = validRate(currentRate);
        if (currentRate > 0.0) lastMovingRate = currentRate;
    }
    lastChangedWorkoutTimeMs = input.workoutTimeMs;
    lastChangedMonotonicTimeMs = input.monotonicTimeMs;
    return currentRate;
}
