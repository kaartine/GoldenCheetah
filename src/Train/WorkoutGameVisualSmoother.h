/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameVisualSmoother_h
#define _GC_WorkoutGameVisualSmoother_h

#include "WorkoutGameSimulation.h"
#include "WorkoutGameWorld.h"

#include <cstdint>

struct WorkoutGameVisualSnapshot
{
    WorkoutGameSimulationSnapshot simulation;
    WorkoutGameWorldSnapshot world;
    WorkoutGameCameraSnapshot camera;
};

class WorkoutGameVisualSmoother
{
public:
    static constexpr std::int64_t TransitionDurationMs = 200;

    void reset();
    void setTarget(
            const WorkoutGameVisualSnapshot &snapshot,
            std::int64_t monotonicTimeMs);
    WorkoutGameVisualSnapshot sample(std::int64_t monotonicTimeMs) const;

private:
    static bool isDiscontinuity(
            const WorkoutGameVisualSnapshot &from,
            const WorkoutGameVisualSnapshot &to);
    static WorkoutGameVisualSnapshot interpolate(
            const WorkoutGameVisualSnapshot &from,
            const WorkoutGameVisualSnapshot &to,
            double amount);

    bool initialized = false;
    std::int64_t transitionStartMs = 0;
    WorkoutGameVisualSnapshot previous;
    WorkoutGameVisualSnapshot target;
};

class WorkoutGameFrameRateCounter
{
public:
    void reset();
    double frameRendered(std::int64_t monotonicTimeMs);
    double framesPerSecond() const { return currentFps; }

private:
    bool initialized = false;
    std::int64_t windowStartMs = 0;
    int frameIntervals = 0;
    double currentFps = 0.0;
};

#endif
