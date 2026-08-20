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

#include <cstddef>
#include <cstdint>
#include <deque>

struct WorkoutGameVisualSnapshot
{
    WorkoutGameSimulationSnapshot simulation;
    WorkoutGameCompetitionSnapshot competition;
    WorkoutGameWorldSnapshot world;
    WorkoutGameCameraSnapshot camera;
    WorkoutGameTerrainTransitionSnapshot terrainTransition;
    std::int64_t presentationTimeMs = 0;
    std::size_t skippedSimulationTicks = 0;
    double riderPedalCycles = 0.0;
};

class WorkoutGameVisualSmoother
{
public:
    static constexpr std::int64_t TransitionDurationMs = 200;
    static constexpr std::int64_t MaximumPredictionMs = 1500;
    static constexpr std::int64_t MinimumSourceIntervalMs = 20;
    static constexpr std::int64_t MaximumSourceIntervalMs = 2000;
    // Absorb one GUI-drain/display phase mismatch while retaining two complete
    // fixed-step snapshots for interpolation.
    static constexpr std::int64_t FixedStepPresentationDelayMs = 40;

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
    bool fixedStepSnapshots = false;
    std::int64_t transitionStartMs = 0;
    std::int64_t lastTargetMonotonicMs = 0;
    std::int64_t sourceIntervalMs = 1000;
    std::int64_t previousPresentationTimeMs = 0;
    std::int64_t targetPresentationTimeMs = 0;
    WorkoutGameVisualSnapshot previous;
    WorkoutGameVisualSnapshot predictionOrigin;
    WorkoutGameVisualSnapshot target;
    std::deque<WorkoutGameVisualSnapshot> fixedStepHistory;
    WorkoutGameTerrainTransition terrainTransition;
};

class WorkoutGameFrameRateCounter
{
public:
    void reset();
    double frameRendered(std::int64_t monotonicTimeMs);
    double frameRenderedNanoseconds(std::int64_t monotonicTimeNs);
    double framesPerSecond() const { return currentFps; }
    double p95FrameIntervalMilliseconds() const { return p95FrameIntervalMs; }

private:
    bool initialized = false;
    std::int64_t lastFrameNs = 0;
    std::int64_t intervalTotalNs = 0;
    std::deque<std::int64_t> recentIntervalsNs;
    double currentFps = 0.0;
    double p95FrameIntervalMs = 0.0;
};

#endif
