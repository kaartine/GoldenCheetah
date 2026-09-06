/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameClock.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

constexpr double MinimumRate = 0.0;
constexpr double MaximumRate = 4.0;
constexpr double MinimumRunningRate = 0.05;
constexpr double CorrectionHorizonMs = 2000.0;
constexpr std::int64_t DiscontinuityThresholdMs = 1000;

double validRate(double rate)
{
    return std::clamp(
            std::isfinite(rate) ? rate : 1.0,
            MinimumRate,
            MaximumRate);
}

}

WorkoutGameClock::WorkoutGameClock(
        std::int64_t requestedStepMs,
        std::size_t requestedMaximumCatchupTicks) :
    stepMs(std::max<std::int64_t>(1, requestedStepMs)),
    maximumCatchupTicks(std::max<std::size_t>(1, requestedMaximumCatchupTicks))
{
}

std::int64_t WorkoutGameClock::monotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t WorkoutGameClock::nextDeadlineAfter(
        std::int64_t monotonicTimeMs) const
{
    const std::int64_t remainder = monotonicTimeMs % stepMs;
    return monotonicTimeMs + (remainder == 0 ? stepMs : stepMs - remainder);
}

void WorkoutGameClock::reset(
        std::int64_t workoutTimeMs,
        std::int64_t monotonicTimeMs,
        bool requestedRunning,
        double rate)
{
    initialized = true;
    running = requestedRunning;
    anchorWorkoutTimeMs = std::max<std::int64_t>(0, workoutTimeMs);
    anchorMonotonicTimeMs = monotonicTimeMs;
    nextDeadlineMonotonicMs = nextDeadlineAfter(monotonicTimeMs);
    lastSourceWorkoutTimeMs = anchorWorkoutTimeMs;
    lastPublishedWorkoutTimeMs = anchorWorkoutTimeMs;
    timelineRate = validRate(rate);
}

std::int64_t WorkoutGameClock::positionAt(
        std::int64_t monotonicTimeMs) const
{
    if (!initialized || !running
            || monotonicTimeMs <= anchorMonotonicTimeMs) {
        return anchorWorkoutTimeMs;
    }
    const double elapsedMs = double(
            monotonicTimeMs - anchorMonotonicTimeMs);
    return std::max<std::int64_t>(
            anchorWorkoutTimeMs,
            anchorWorkoutTimeMs
                + std::int64_t(std::llround(elapsedMs * timelineRate)));
}

void WorkoutGameClock::setAnchor(
        std::int64_t requestedWorkoutTimeMs,
        std::int64_t monotonicTimeMs,
        bool requestedRunning,
        double rateHint)
{
    const std::int64_t workoutTimeMs = std::max<std::int64_t>(
            0, requestedWorkoutTimeMs);
    if (!initialized || monotonicTimeMs < anchorMonotonicTimeMs) {
        reset(workoutTimeMs, monotonicTimeMs, requestedRunning, rateHint);
        return;
    }

    const bool runningChanged = running != requestedRunning;
    const std::int64_t predicted = positionAt(monotonicTimeMs);
    const bool sourceReversed = lastSourceWorkoutTimeMs
                > DiscontinuityThresholdMs
            && workoutTimeMs < lastSourceWorkoutTimeMs
                - DiscontinuityThresholdMs;
    const bool largeForwardCorrection = workoutTimeMs
            > predicted + DiscontinuityThresholdMs;
    if (sourceReversed || largeForwardCorrection) {
        reset(workoutTimeMs, monotonicTimeMs, requestedRunning, rateHint);
        return;
    }

    const double correctionRate = double(workoutTimeMs - predicted)
            / CorrectionHorizonMs;
    anchorWorkoutTimeMs = std::max(
            predicted, lastPublishedWorkoutTimeMs);
    anchorMonotonicTimeMs = monotonicTimeMs;
    const double hintedRate = validRate(rateHint);
    double correctedRate = hintedRate + correctionRate;
    if (requestedRunning && hintedRate > 0.0) {
        correctedRate = std::max(
                correctedRate,
                std::max(MinimumRunningRate, hintedRate * 0.5));
    }
    timelineRate = validRate(correctedRate);
    lastSourceWorkoutTimeMs = std::max(
            lastSourceWorkoutTimeMs, workoutTimeMs);
    running = requestedRunning;
    if (runningChanged) {
        nextDeadlineMonotonicMs = nextDeadlineAfter(monotonicTimeMs);
    }
    if (!running) {
        anchorWorkoutTimeMs = std::max(
                workoutTimeMs, lastPublishedWorkoutTimeMs);
    }
}

void WorkoutGameClock::setRunning(
        bool requestedRunning,
        std::int64_t monotonicTimeMs)
{
    if (!initialized) {
        reset(0, monotonicTimeMs, requestedRunning, 1.0);
        return;
    }
    if (running == requestedRunning) return;

    anchorWorkoutTimeMs = std::max(
            positionAt(monotonicTimeMs), lastPublishedWorkoutTimeMs);
    anchorMonotonicTimeMs = monotonicTimeMs;
    running = requestedRunning;
    nextDeadlineMonotonicMs = nextDeadlineAfter(monotonicTimeMs);
}

WorkoutGameClockAdvance WorkoutGameClock::advance(
        std::int64_t monotonicTimeMs)
{
    WorkoutGameClockAdvance result;
    if (!initialized || !running
            || monotonicTimeMs < nextDeadlineMonotonicMs) {
        return result;
    }

    const std::size_t dueTicks = std::size_t(
            (monotonicTimeMs - nextDeadlineMonotonicMs) / stepMs + 1);
    result.skippedTicks = dueTicks > maximumCatchupTicks
            ? dueTicks - maximumCatchupTicks : 0;
    const std::int64_t firstDeadline = nextDeadlineMonotonicMs
            + std::int64_t(result.skippedTicks) * stepMs;
    const std::size_t producedTicks = dueTicks - result.skippedTicks;
    result.ticks.reserve(producedTicks);
    for (std::size_t index = 0; index < producedTicks; ++index) {
        const std::int64_t deadline = firstDeadline
                + std::int64_t(index) * stepMs;
        const std::int64_t workoutTimeMs = std::max(
                positionAt(deadline), lastPublishedWorkoutTimeMs);
        result.ticks.push_back({deadline, workoutTimeMs});
        lastPublishedWorkoutTimeMs = workoutTimeMs;
    }
    nextDeadlineMonotonicMs += std::int64_t(dueTicks) * stepMs;
    return result;
}
