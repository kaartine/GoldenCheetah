/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameDiagnostics.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr double MovementEpsilonMeters = 1e-6;
constexpr double VisibleAirborneHeightMeters = 0.08;
constexpr std::int64_t LateFrameThresholdMs = 25;
constexpr double LongPresentationWorkThresholdMs = 8.0;
constexpr std::int64_t NanosecondsPerMillisecond = 1000000ll;
constexpr std::int64_t LateFrameThresholdNs =
        LateFrameThresholdMs * NanosecondsPerMillisecond;

double milliseconds(std::int64_t nanoseconds)
{
    return double(std::max<std::int64_t>(0, nanoseconds))
            / double(NanosecondsPerMillisecond);
}

}

void WorkoutGameVisualRevisionTracker::markChanged() noexcept
{
    pendingRevision.fetch_add(1, std::memory_order_release);
}

void WorkoutGameVisualRevisionTracker::synchronize() noexcept
{
    synchronizedRevision.store(
            pendingRevision.load(std::memory_order_acquire),
            std::memory_order_release);
}

std::uint64_t WorkoutGameVisualRevisionTracker::presented() const noexcept
{
    return synchronizedRevision.load(std::memory_order_acquire);
}

void WorkoutGameColdStartFrameCapture::start(
        std::int64_t startTimeNs,
        std::uint64_t initialVisualRevision) noexcept
{
    recording.store(false, std::memory_order_release);
    measurementStartNs.store(
            std::max<std::int64_t>(0, startTimeNs),
            std::memory_order_relaxed);
    initialRevision.store(initialVisualRevision, std::memory_order_relaxed);
    recordedFrames.store(0, std::memory_order_relaxed);
    droppedFrames.store(0, std::memory_order_relaxed);
    recording.store(true, std::memory_order_release);
}

void WorkoutGameColdStartFrameCapture::stop() noexcept
{
    recording.store(false, std::memory_order_release);
}

void WorkoutGameColdStartFrameCapture::recordFrame(
        std::int64_t presentationTimeNs,
        std::uint64_t visualRevision) noexcept
{
    if (!recording.load(std::memory_order_acquire)) return;
    const std::int64_t start = measurementStartNs.load(
            std::memory_order_relaxed);
    if (presentationTimeNs < start
            || presentationTimeNs > start + DurationNs) {
        return;
    }
    const std::uint32_t index = recordedFrames.fetch_add(
            1, std::memory_order_acq_rel);
    if (index >= Capacity) {
        droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    samples[index].visualRevision.store(
            visualRevision, std::memory_order_relaxed);
    samples[index].presentationTimeNs.store(
            presentationTimeNs, std::memory_order_release);
}

WorkoutGameColdStartFrameSnapshot
WorkoutGameColdStartFrameCapture::snapshot(
        std::int64_t currentTimeNs) const noexcept
{
    WorkoutGameColdStartFrameSnapshot result;
    result.active = recording.load(std::memory_order_acquire);
    const std::int64_t start = measurementStartNs.load(
            std::memory_order_relaxed);
    const std::int64_t end = std::clamp(
            currentTimeNs, start, start + DurationNs);
    result.complete = currentTimeNs - start >= DurationNs;
    result.droppedFrameCount = droppedFrames.load(
            std::memory_order_relaxed);

    const std::size_t count = std::min<std::size_t>(
            recordedFrames.load(std::memory_order_acquire), Capacity);
    std::array<std::int64_t, Capacity> intervals{};
    std::size_t intervalCount = 0;
    std::int64_t previousTime = start;
    std::int64_t unchangedSince = start;
    std::uint64_t revision = initialRevision.load(std::memory_order_relaxed);
    std::uint32_t visualChanges = 0;
    std::uint32_t consecutiveLate = 0;

    for (std::size_t index = 0; index < count; ++index) {
        const std::int64_t timestamp = samples[index].presentationTimeNs.load(
                std::memory_order_acquire);
        if (timestamp < start || timestamp > end || timestamp < previousTime) {
            continue;
        }
        const std::int64_t interval = timestamp - previousTime;
        intervals[intervalCount++] = interval;
        result.maximumFrameIntervalMs = std::max(
                result.maximumFrameIntervalMs, milliseconds(interval));
        if (interval > LateFrameThresholdNs) {
            ++consecutiveLate;
            result.maximumConsecutiveLateFrames = std::max(
                    result.maximumConsecutiveLateFrames, consecutiveLate);
        } else {
            consecutiveLate = 0;
        }
        const std::uint64_t sampleRevision =
                samples[index].visualRevision.load(std::memory_order_relaxed);
        if (sampleRevision != revision) {
            result.longestUnchangedVisualIntervalMs = std::max(
                    result.longestUnchangedVisualIntervalMs,
                    milliseconds(timestamp - unchangedSince));
            unchangedSince = timestamp;
            revision = sampleRevision;
            ++visualChanges;
        }
        previousTime = timestamp;
        ++result.frameCount;
    }

    result.longestUnchangedVisualIntervalMs = std::max(
            result.longestUnchangedVisualIntervalMs,
            milliseconds(end - unchangedSince));
    if (result.frameCount > 0) {
        result.startToFirstSwapMs = milliseconds(intervals[0]);
    }
    if (intervalCount > 0) {
        std::sort(intervals.begin(), intervals.begin() + intervalCount);
        const std::size_t p99 = std::min(
                intervalCount - 1,
                std::size_t(std::ceil(double(intervalCount) * 0.99)) - 1);
        result.p99FrameIntervalMs = milliseconds(intervals[p99]);
    }
    const double elapsedSeconds = double(std::max<std::int64_t>(1, end - start))
            / 1000000000.0;
    result.swapFramesPerSecond = double(result.frameCount) / elapsedSeconds;
    result.uniqueVisualFramesPerSecond = double(visualChanges) / elapsedSeconds;
    return result;
}

void WorkoutGameDiagnostics::reset()
{
    initialized = false;
    frameTimingInitialized = false;
    previousRoadDistanceMeters = 0.0;
    previousMonotonicTimeMs = 0;
    previousRenderedWorkoutTimeMs = 0;
    backwardFrameCount = 0;
    stationaryFrameCount = 0;
    lateFrameCount = 0;
    longPresentationWorkCount = 0;
    unexpectedAirborneFrameCount = 0;
    largestFrameIntervalMs = 0;
    largestPresentationWorkMs = 0.0;
    largestRegressionMeters = 0.0;
}

WorkoutGameDiagnosticsSnapshot WorkoutGameDiagnostics::update(
        const WorkoutGameDiagnosticsInput &input)
{
    WorkoutGameDiagnosticsSnapshot result;
    result.input = input;
    if (!input.ready || !input.sessionRunning) {
        reset();
        return result;
    }

    const bool timelineReset = initialized
            && input.renderedWorkoutTimeMs < previousRenderedWorkoutTimeMs;
    if (timelineReset) reset();
    if (!initialized) {
        initialized = true;
        previousRoadDistanceMeters = input.renderedRoadDistanceMeters;
        previousMonotonicTimeMs = input.monotonicTimeMs;
        previousRenderedWorkoutTimeMs = input.renderedWorkoutTimeMs;
        frameTimingInitialized = input.frameTimingWarmupComplete;
        result.ready = true;
        result.backwardFrameCount = backwardFrameCount;
        result.stationaryFrameCount = stationaryFrameCount;
        result.lateFrameCount = lateFrameCount;
        result.longPresentationWorkCount = longPresentationWorkCount;
        result.unexpectedAirborneFrameCount = unexpectedAirborneFrameCount;
        result.largestFrameIntervalMs = largestFrameIntervalMs;
        result.largestPresentationWorkMs = largestPresentationWorkMs;
        result.largestRegressionMeters = largestRegressionMeters;
        return result;
    }

    result.ready = true;
    if (!input.frameTimingWarmupComplete) {
        frameTimingInitialized = false;
    } else if (!frameTimingInitialized) {
        frameTimingInitialized = true;
    } else {
        result.frameIntervalMs = std::max<std::int64_t>(
                0, input.monotonicTimeMs - previousMonotonicTimeMs);
        largestFrameIntervalMs = std::max(
                largestFrameIntervalMs, result.frameIntervalMs);
        if (result.frameIntervalMs > LateFrameThresholdMs) ++lateFrameCount;
    }
    const double presentationWorkMs =
            std::isfinite(input.presentationWorkMs)
            ? std::max(0.0, input.presentationWorkMs) : 0.0;
    largestPresentationWorkMs = std::max(
            largestPresentationWorkMs, presentationWorkMs);
    if (presentationWorkMs > LongPresentationWorkThresholdMs) {
        ++longPresentationWorkCount;
    }
    if (input.worldReady && input.riderAirborne
            && input.airHeightMeters >= VisibleAirborneHeightMeters
            && !input.airborneExpected) {
        ++unexpectedAirborneFrameCount;
    }
    result.frameDistanceDeltaMeters = input.renderedRoadDistanceMeters
            - previousRoadDistanceMeters;
    if (input.movingForward
            && result.frameDistanceDeltaMeters < -MovementEpsilonMeters) {
        ++backwardFrameCount;
        largestRegressionMeters = std::max(
                largestRegressionMeters,
                -result.frameDistanceDeltaMeters);
    } else if (input.movingForward
            && std::abs(result.frameDistanceDeltaMeters)
                <= MovementEpsilonMeters) {
        ++stationaryFrameCount;
    }
    result.backwardFrameCount = backwardFrameCount;
    result.stationaryFrameCount = stationaryFrameCount;
    result.lateFrameCount = lateFrameCount;
    result.longPresentationWorkCount = longPresentationWorkCount;
    result.unexpectedAirborneFrameCount = unexpectedAirborneFrameCount;
    result.largestFrameIntervalMs = largestFrameIntervalMs;
    result.largestPresentationWorkMs = largestPresentationWorkMs;
    result.largestRegressionMeters = largestRegressionMeters;
    previousRoadDistanceMeters = input.renderedRoadDistanceMeters;
    previousMonotonicTimeMs = input.monotonicTimeMs;
    previousRenderedWorkoutTimeMs = input.renderedWorkoutTimeMs;
    return result;
}
