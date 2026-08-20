/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameEngine_h
#define _GC_WorkoutGameEngine_h

#include "WorkoutGameFeatureRuntime.h"
#include "WorkoutGameVisualSmoother.h"

struct WorkoutGameEngineInput
{
    WorkoutGameSimulationInput simulation;
    int heartRate = 0;
    std::int64_t telemetryMonotonicTimeMs = -1;
};

struct WorkoutGameEngineFrame
{
    WorkoutGameVisualSnapshot visual;
    double watts = 0.0;
    double targetWatts = 0.0;
    int cadenceRpm = 0;
    int heartRate = 0;
    int virtualGear = 1;
    std::uint64_t sequence = 0;
    std::size_t skippedTicks = 0;
    bool telemetryStale = false;
};

class WorkoutGameEngine
{
public:
    bool configure(
            const WorkoutGameCourse &course,
            double ftpWatts,
            bool featureLabEnabled);
    void reset();
    WorkoutGameEngineFrame update(
            const WorkoutGameEngineInput &input,
            std::int64_t presentationTimeMs,
            std::size_t skippedTicks = 0);
    void resynchronize(
            const WorkoutGameEngineInput &input,
            std::int64_t presentationTimeMs,
            std::size_t skippedTicks);

private:
    WorkoutGameCourse course;
    double ftpWatts = 0.0;
    bool featureLabEnabled = false;
    bool configured = false;
    WorkoutGameRoadCourse roadCourse;
    bool worldClockInitialized = false;
    std::int64_t lastWorldTimeMs = 0;
    double riderPedalCycles = 0.0;
    std::uint64_t sequence = 0;
    WorkoutGameSimulation simulation;
    WorkoutGameFeatureRuntime featureRuntime;
    WorkoutGamePhysics physics;
    WorkoutGameCamera camera;
};

#endif
