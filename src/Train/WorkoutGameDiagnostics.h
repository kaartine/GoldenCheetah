/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameDiagnostics_h
#define _GC_WorkoutGameDiagnostics_h

#include <cstddef>
#include <cstdint>

struct WorkoutGameDiagnosticsInput
{
    bool ready = false;
    bool sessionRunning = false;
    bool movingForward = false;
    std::uint64_t frameNumber = 0;
    std::int64_t monotonicTimeMs = 0;
    std::int64_t sourceWorkoutTimeMs = 0;
    std::int64_t renderedWorkoutTimeMs = 0;
    int sourceSection = -1;
    int renderedSection = -1;
    double sourceSectionProgress = 0.0;
    double renderedSectionProgress = 0.0;
    double sourceRoadDistanceMeters = 0.0;
    double renderedRoadDistanceMeters = 0.0;
    double framesPerSecond = 0.0;
    double p50FrameIntervalMs = 0.0;
    double p95FrameIntervalMs = 0.0;
    double p99FrameIntervalMs = 0.0;
    std::size_t skippedSimulationTicks = 0;
    int rendererQueueDepth = 0;
    double presentationWorkMs = 0.0;
    bool worldReady = false;
    bool riderAirborne = false;
    bool airborneExpected = false;
    double riderElevationMeters = 0.0;
    double surfaceElevationMeters = 0.0;
    double riderClearanceMeters = 0.0;
    double airHeightMeters = 0.0;
    double lateralOffsetMeters = 0.0;
    double visibleElevationChangeMeters = 0.0;
    double renderedGradePercent = 0.0;
};

struct WorkoutGameDiagnosticsSnapshot
{
    bool ready = false;
    WorkoutGameDiagnosticsInput input;
    std::int64_t frameIntervalMs = 0;
    std::int64_t largestFrameIntervalMs = 0;
    double frameDistanceDeltaMeters = 0.0;
    std::uint64_t backwardFrameCount = 0;
    std::uint64_t stationaryFrameCount = 0;
    std::uint64_t lateFrameCount = 0;
    std::uint64_t longPresentationWorkCount = 0;
    std::uint64_t unexpectedAirborneFrameCount = 0;
    double largestPresentationWorkMs = 0.0;
    double largestRegressionMeters = 0.0;
};

class WorkoutGameDiagnostics
{
public:
    void reset();
    WorkoutGameDiagnosticsSnapshot update(
            const WorkoutGameDiagnosticsInput &input);

private:
    bool initialized = false;
    double previousRoadDistanceMeters = 0.0;
    std::int64_t previousMonotonicTimeMs = 0;
    std::int64_t previousRenderedWorkoutTimeMs = 0;
    std::uint64_t backwardFrameCount = 0;
    std::uint64_t stationaryFrameCount = 0;
    std::uint64_t lateFrameCount = 0;
    std::uint64_t longPresentationWorkCount = 0;
    std::uint64_t unexpectedAirborneFrameCount = 0;
    std::int64_t largestFrameIntervalMs = 0;
    double largestPresentationWorkMs = 0.0;
    double largestRegressionMeters = 0.0;
};

#endif
