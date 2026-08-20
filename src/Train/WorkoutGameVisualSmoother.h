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

#include "WorkoutGameCompetition.h"
#include "WorkoutGameSimulation.h"
#include "WorkoutGameTerrainTransition.h"
#include "WorkoutGameWorld.h"

#include <cstdint>

struct WorkoutGameVisualSnapshot
{
    WorkoutGameSimulationSnapshot simulation;
    WorkoutGameCompetitionSnapshot competition;
    WorkoutGameWorldSnapshot world;
    WorkoutGameCameraSnapshot camera;
    WorkoutGameTerrainTransitionSnapshot terrainTransition;
};

class WorkoutGameVisualSmoother
{
public:
    static constexpr std::int64_t TransitionDurationMs = 200;
    static constexpr std::int64_t MaximumPredictionMs = 1500;
    static constexpr std::int64_t MinimumSourceIntervalMs = 20;
    static constexpr std::int64_t MaximumSourceIntervalMs = 2000;

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
    bool sourceAdvancing = false;
    std::int64_t transitionStartMs = 0;
    std::int64_t lastTargetMonotonicMs = 0;
    std::int64_t sourceIntervalMs = 1000;
    WorkoutGameVisualSnapshot previous;
    WorkoutGameVisualSnapshot predictionOrigin;
    WorkoutGameVisualSnapshot target;
    WorkoutGameTerrainTransition terrainTransition;
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
