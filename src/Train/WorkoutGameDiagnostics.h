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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

struct WorkoutGameColdStartFrameSnapshot
{
    bool active = false;
    bool complete = false;
    std::uint32_t frameCount = 0;
    std::uint32_t droppedFrameCount = 0;
    double swapFramesPerSecond = 0.0;
    double uniqueVisualFramesPerSecond = 0.0;
    double startToFirstSwapMs = 0.0;
    double p99FrameIntervalMs = 0.0;
    double maximumFrameIntervalMs = 0.0;
    std::uint32_t maximumConsecutiveLateFrames = 0;
    double longestUnchangedVisualIntervalMs = 0.0;
};

class WorkoutGameColdStartFrameCapture
{
public:
    static constexpr std::size_t Capacity = 4096;
    static constexpr std::int64_t DurationNs = 10000000000ll;

    void start(
            std::int64_t startTimeNs,
            std::uint64_t initialVisualRevision) noexcept;
    void stop() noexcept;
    void recordFrame(
            std::int64_t presentationTimeNs,
            std::uint64_t visualRevision) noexcept;
    WorkoutGameColdStartFrameSnapshot snapshot(
            std::int64_t currentTimeNs) const noexcept;

private:
    struct Sample
    {
        std::atomic<std::int64_t> presentationTimeNs{0};
        std::atomic<std::uint64_t> visualRevision{0};
    };

    std::array<Sample, Capacity> samples;
    std::atomic<std::int64_t> measurementStartNs{0};
    std::atomic<std::uint64_t> initialRevision{0};
    std::atomic<std::uint32_t> recordedFrames{0};
    std::atomic<std::uint32_t> droppedFrames{0};
    std::atomic_bool recording{false};
};

struct WorkoutGameDiagnosticsInput
{
    bool ready = false;
    bool sessionRunning = false;
    bool movingForward = false;
    bool frameTimingWarmupComplete = true;
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
    WorkoutGameColdStartFrameSnapshot coldStart;
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
    bool frameTimingInitialized = false;
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
