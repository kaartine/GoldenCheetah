/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGame3DCameraPresentation_h
#define _GC_WorkoutGame3DCameraPresentation_h

#include <cstdint>

enum class WorkoutGame3DCameraPresentationMode
{
    Chase,
    OpeningSide,
    IdleSide,
    ReturningToChase
};

struct WorkoutGame3DCameraPresentationInput
{
    std::int64_t workoutTimeMs = 0;
    double watts = 0.0;
    double cadenceRpm = 0.0;
    bool featureCritical = false;
    bool airborne = false;
};

struct WorkoutGame3DCameraPresentationSnapshot
{
    WorkoutGame3DCameraPresentationMode mode =
            WorkoutGame3DCameraPresentationMode::Chase;
    double sideBlend = 0.0;
};

class WorkoutGame3DCameraPresentation
{
public:
    static constexpr std::int64_t OpeningHoldMs = 1000;
    static constexpr std::int64_t OpeningTransitionMs = 2000;
    static constexpr std::int64_t IdleDelayMs = 4000;
    static constexpr std::int64_t IdleTransitionMs = 1800;
    static constexpr std::int64_t ChaseReturnMs = 1200;

    void reset();
    WorkoutGame3DCameraPresentationSnapshot update(
            const WorkoutGame3DCameraPresentationInput &input);
    const WorkoutGame3DCameraPresentationSnapshot &snapshot() const
    {
        return current;
    }

private:
    static double transition(
            std::int64_t elapsedMs,
            std::int64_t durationMs);
    void begin(std::int64_t workoutTimeMs);

    bool initialized = false;
    bool openingCancelled = false;
    std::int64_t sessionStartMs = 0;
    std::int64_t lastWorkoutTimeMs = 0;
    std::int64_t idleStartMs = -1;
    std::int64_t returnStartMs = -1;
    double returnStartBlend = 0.0;
    WorkoutGame3DCameraPresentationSnapshot current;
};

#endif
