/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DCameraPresentation.h"

#include <algorithm>
#include <cmath>

void WorkoutGame3DCameraPresentation::reset()
{
    initialized = false;
    openingCancelled = false;
    sessionStartMs = 0;
    lastWorkoutTimeMs = 0;
    idleStartMs = -1;
    returnStartMs = -1;
    returnStartBlend = 0.0;
    current = {};
}

double WorkoutGame3DCameraPresentation::transition(
        std::int64_t elapsedMs,
        std::int64_t durationMs)
{
    const double linear = durationMs > 0
            ? std::clamp(double(elapsedMs) / double(durationMs), 0.0, 1.0)
            : 1.0;
    return linear * linear * (3.0 - 2.0 * linear);
}

void WorkoutGame3DCameraPresentation::begin(std::int64_t workoutTimeMs)
{
    initialized = true;
    openingCancelled = false;
    sessionStartMs = workoutTimeMs;
    lastWorkoutTimeMs = workoutTimeMs;
    idleStartMs = -1;
    returnStartMs = -1;
    returnStartBlend = 0.0;
    current.mode = WorkoutGame3DCameraPresentationMode::OpeningSide;
    current.sideBlend = 1.0;
}

WorkoutGame3DCameraPresentationSnapshot
WorkoutGame3DCameraPresentation::update(
        const WorkoutGame3DCameraPresentationInput &input)
{
    const std::int64_t now = std::max<std::int64_t>(0, input.workoutTimeMs);
    if (!initialized || now < lastWorkoutTimeMs) begin(now);
    lastWorkoutTimeMs = now;

    if (input.featureCritical || input.airborne) openingCancelled = true;
    const std::int64_t openingElapsed = now - sessionStartMs;
    if (!openingCancelled
            && openingElapsed < OpeningHoldMs + OpeningTransitionMs) {
        current.mode = WorkoutGame3DCameraPresentationMode::OpeningSide;
        current.sideBlend = openingElapsed <= OpeningHoldMs
                ? 1.0
                : 1.0 - transition(
                    openingElapsed - OpeningHoldMs, OpeningTransitionMs);
        idleStartMs = -1;
        returnStartMs = -1;
        return current;
    }
    if (current.mode == WorkoutGame3DCameraPresentationMode::OpeningSide) {
        current.mode = WorkoutGame3DCameraPresentationMode::Chase;
        current.sideBlend = 0.0;
    }

    const bool stoppedPedalling = std::isfinite(input.watts)
            && std::isfinite(input.cadenceRpm)
            && input.watts < 20.0
            && input.cadenceRpm <= 3.0
            && !input.featureCritical
            && !input.airborne;
    if (stoppedPedalling) {
        if (idleStartMs < 0) idleStartMs = now;
        returnStartMs = -1;
        const std::int64_t idleElapsed = now - idleStartMs;
        if (idleElapsed < IdleDelayMs) {
            current.mode = WorkoutGame3DCameraPresentationMode::Chase;
            current.sideBlend = 0.0;
        } else {
            current.mode = WorkoutGame3DCameraPresentationMode::IdleSide;
            current.sideBlend = transition(
                    idleElapsed - IdleDelayMs, IdleTransitionMs);
        }
        return current;
    }

    idleStartMs = -1;
    if (current.sideBlend <= 0.0) {
        returnStartMs = -1;
        returnStartBlend = 0.0;
        current.mode = WorkoutGame3DCameraPresentationMode::Chase;
        current.sideBlend = 0.0;
        return current;
    }
    if (returnStartMs < 0) {
        returnStartMs = now;
        returnStartBlend = current.sideBlend;
    }
    current.sideBlend = returnStartBlend * (1.0 - transition(
            now - returnStartMs, ChaseReturnMs));
    current.mode = current.sideBlend > 0.0
            ? WorkoutGame3DCameraPresentationMode::ReturningToChase
            : WorkoutGame3DCameraPresentationMode::Chase;
    return current;
}
