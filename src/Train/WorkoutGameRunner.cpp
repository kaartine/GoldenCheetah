/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameRunner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace {

using MonotonicClock = std::chrono::steady_clock;

MonotonicClock::time_point timePoint(std::int64_t milliseconds)
{
    return MonotonicClock::time_point(std::chrono::milliseconds(milliseconds));
}

void lowerWorkerPriority()
{
#if defined(__linux__)
    // Workout Game is optional presentation. Device I/O, recording and trainer
    // control remain on the application's normal-priority threads.
    setpriority(PRIO_PROCESS, 0, 5);
#endif
}

}

WorkoutGameRunner::WorkoutGameRunner() : clock(20, 4)
{
}

WorkoutGameRunner::~WorkoutGameRunner()
{
    shutdown();
}

std::int64_t WorkoutGameRunner::monotonicMilliseconds()
{
    return WorkoutGameClock::monotonicMilliseconds();
}

bool WorkoutGameRunner::configure(
        const WorkoutGameCourse &course,
        double ftpWatts,
        bool featureLabEnabled)
{
    shutdown();
    clearOutput();
    configured = course.status == WorkoutGameCourseStatus::Ready
            && !course.sections.empty()
            && course.durationMs > 0
            && std::isfinite(ftpWatts)
            && ftpWatts > 0.0;
    if (!configured) return false;

    configuredCourse = course;
    configuredFtpWatts = ftpWatts;
    configuredFeatureLabEnabled = featureLabEnabled;
    const std::int64_t nowMs = monotonicMilliseconds();
    inputState = InputState();
    inputState.anchorMonotonicTimeMs = nowMs;
    inputState.generation = lifecycleGeneration.fetch_add(
            1, std::memory_order_acq_rel) + 1;
    inputState.revision = 1;
    inputState.anchorRevision = 1;
    inputState.resetRevision = 1;
    stopping = false;
    ensureThread();
    return true;
}

void WorkoutGameRunner::ensureThread()
{
    if (!configured || worker.joinable()) return;
    worker = std::thread(&WorkoutGameRunner::run, this);
}

void WorkoutGameRunner::start(
        std::int64_t workoutTimeMs,
        double rateHint)
{
    if (!configured) return;
    ensureThread();
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        inputState.generation = lifecycleGeneration.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        inputState.anchorWorkoutTimeMs = std::max<std::int64_t>(0, workoutTimeMs);
        inputState.anchorMonotonicTimeMs = monotonicMilliseconds();
        inputState.rateHint = rateHint;
        inputState.running = true;
        inputState.input.simulation.paused = false;
        ++inputState.anchorRevision;
        ++inputState.resetRevision;
        ++inputState.revision;
    }
    inputChanged.notify_one();
}

void WorkoutGameRunner::resume(
        std::int64_t workoutTimeMs,
        double rateHint)
{
    if (!configured) return;
    ensureThread();
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        inputState.generation = lifecycleGeneration.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        inputState.anchorWorkoutTimeMs = std::max<std::int64_t>(0, workoutTimeMs);
        inputState.anchorMonotonicTimeMs = monotonicMilliseconds();
        inputState.rateHint = rateHint;
        inputState.running = true;
        inputState.input.simulation.paused = false;
        ++inputState.anchorRevision;
        ++inputState.revision;
    }
    inputChanged.notify_one();
}

void WorkoutGameRunner::setAnchor(
        std::int64_t workoutTimeMs,
        double rateHint)
{
    if (!configured) return;
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        inputState.anchorWorkoutTimeMs = std::max<std::int64_t>(0, workoutTimeMs);
        inputState.anchorMonotonicTimeMs = monotonicMilliseconds();
        inputState.rateHint = rateHint;
        ++inputState.anchorRevision;
        ++inputState.revision;
    }
    inputChanged.notify_one();
}

void WorkoutGameRunner::setTelemetry(const WorkoutGameEngineInput &input)
{
    if (!configured) return;
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        const std::int64_t currentWorkoutTime =
                inputState.input.simulation.workoutTimeMs;
        const bool paused = inputState.input.simulation.paused;
        inputState.input = input;
        inputState.input.simulation.workoutTimeMs = currentWorkoutTime;
        inputState.input.simulation.paused = paused;
        ++inputState.revision;
    }
    inputChanged.notify_one();
}

void WorkoutGameRunner::pause(std::int64_t workoutTimeMs)
{
    if (!configured) return;
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        inputState.generation = lifecycleGeneration.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        inputState.anchorWorkoutTimeMs = std::max<std::int64_t>(0, workoutTimeMs);
        inputState.anchorMonotonicTimeMs = monotonicMilliseconds();
        inputState.running = false;
        inputState.input.simulation.paused = true;
        ++inputState.anchorRevision;
        ++inputState.revision;
    }
    clearOutput();
    inputChanged.notify_one();
}

void WorkoutGameRunner::stop(std::int64_t workoutTimeMs)
{
    pause(workoutTimeMs);
}

bool WorkoutGameRunner::takeLatest(WorkoutGameEngineFrame &frame)
{
    std::lock_guard<std::mutex> lock(outputMutex);
    if (latestFrame.sequence == 0
            || latestFrame.sequence == consumedSequence) {
        return false;
    }
    frame = latestFrame;
    consumedSequence = latestFrame.sequence;
    return true;
}

void WorkoutGameRunner::clearOutput()
{
    std::lock_guard<std::mutex> lock(outputMutex);
    latestFrame = WorkoutGameEngineFrame();
    consumedSequence = publicationSequence;
}

bool WorkoutGameRunner::publish(
        WorkoutGameEngineFrame frame,
        std::uint64_t expectedGeneration)
{
    std::lock_guard<std::mutex> lock(outputMutex);
    if (expectedGeneration != 0
            && lifecycleGeneration.load(std::memory_order_acquire)
                != expectedGeneration) {
        return false;
    }
    frame.sequence = ++publicationSequence;
    latestFrame = std::move(frame);
    return true;
}

void WorkoutGameRunner::run()
{
    lowerWorkerPriority();
    if (!engine.configure(
            configuredCourse,
            configuredFtpWatts,
            configuredFeatureLabEnabled)) {
        return;
    }
    std::uint64_t seenRevision = 0;
    std::uint64_t seenAnchorRevision = 0;
    std::uint64_t seenResetRevision = 0;
    std::size_t totalSkippedTicks = 0;
    InputState state;

    while (true) {
        {
            std::unique_lock<std::mutex> lock(inputMutex);
            if (seenRevision == inputState.revision && !stopping) {
                if (clock.isRunning()) {
                    inputChanged.wait_until(
                            lock,
                            timePoint(clock.nextDeadlineMonotonicMilliseconds()),
                            [this, seenRevision]() {
                                return stopping
                                        || inputState.revision != seenRevision;
                            });
                } else {
                    inputChanged.wait(lock, [this, seenRevision]() {
                        return stopping || inputState.revision != seenRevision;
                    });
                }
            }
            if (stopping) return;
            state = inputState;
            seenRevision = state.revision;
        }

        if (state.resetRevision != seenResetRevision) {
            engine.reset();
            totalSkippedTicks = 0;
            clock.reset(
                    state.anchorWorkoutTimeMs,
                    state.anchorMonotonicTimeMs,
                    state.running,
                    state.rateHint);
            seenResetRevision = state.resetRevision;
            seenAnchorRevision = state.anchorRevision;
        } else if (state.anchorRevision != seenAnchorRevision) {
            clock.setAnchor(
                    state.anchorWorkoutTimeMs,
                    state.anchorMonotonicTimeMs,
                    state.running,
                    state.rateHint);
            seenAnchorRevision = state.anchorRevision;
        } else if (clock.isRunning() != state.running) {
            clock.setRunning(state.running, monotonicMilliseconds());
        }

        const std::int64_t nowMs = monotonicMilliseconds();
        const WorkoutGameClockAdvance advance = clock.advance(nowMs);
        totalSkippedTicks += advance.skippedTicks;
        std::size_t firstTick = 0;
        if (advance.skippedTicks > 0 && !advance.ticks.empty()) {
            WorkoutGameEngineInput skipInput = state.input;
            skipInput.simulation.workoutTimeMs =
                    advance.ticks.front().workoutTimeMs;
            engine.resynchronize(
                    skipInput,
                    advance.ticks.front().deadlineMonotonicMs,
                    totalSkippedTicks);
            firstTick = 1;
        }
        for (std::size_t index = firstTick;
             index < advance.ticks.size(); ++index) {
            const WorkoutGameClockTick &tick = advance.ticks[index];
            WorkoutGameEngineInput tickInput = state.input;
            tickInput.simulation.workoutTimeMs = tick.workoutTimeMs;
            tickInput.simulation.paused = false;
            if (!publish(
                    engine.update(
                        tickInput,
                        tick.deadlineMonotonicMs,
                        totalSkippedTicks),
                    state.generation)) {
                break;
            }
        }
    }
}

void WorkoutGameRunner::shutdown()
{
    lifecycleGeneration.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        stopping = true;
        ++inputState.revision;
    }
    inputChanged.notify_all();
    if (worker.joinable()) worker.join();
    configured = false;
    stopping = false;
    clearOutput();
}
