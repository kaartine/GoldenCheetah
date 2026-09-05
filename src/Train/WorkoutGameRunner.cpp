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
constexpr std::int64_t MaximumTelemetryAgeMs = 2000;

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

bool expireTelemetry(
        WorkoutGameEngineInput &input,
        std::int64_t tickMonotonicTimeMs)
{
    const bool stale = input.telemetryMonotonicTimeMs < 0
            || tickMonotonicTimeMs - input.telemetryMonotonicTimeMs
                > MaximumTelemetryAgeMs;
    if (!stale) return false;
    input.simulation.actualWatts = 0.0;
    input.simulation.cadenceRpm = 0.0;
    input.simulation.authoritativeSpeedKph = -1.0;
    input.simulation.drivetrainSpeedLimitKph = -1.0;
    input.heartRate = 0;
    return true;
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
    return configure(course, ftpWatts, featureLabEnabled, std::nullopt);
}

bool WorkoutGameRunner::configure(
        const WorkoutGameCourse &course,
        double ftpWatts,
        bool featureLabEnabled,
        std::optional<WorkoutGameFeatureLabGapScenario> gapScenario)
{
    shutdown();
    clearOutput();
    configuredFeatureLabGapScenario.reset();
    configured = course.status == WorkoutGameCourseStatus::Ready
            && !course.sections.empty()
            && course.durationMs > 0
            && std::isfinite(ftpWatts)
            && ftpWatts > 0.0;
    if (!configured) return false;

    configuredCourse = course;
    configuredFtpWatts = ftpWatts;
    configuredFeatureLabEnabled = featureLabEnabled;
    if (featureLabEnabled) {
        configuredFeatureLabGapScenario = gapScenario;
    }
    const std::int64_t nowMs = monotonicMilliseconds();
    inputState = InputState();
    inputState.anchorMonotonicTimeMs = nowMs;
    inputState.generation = lifecycleGeneration.fetch_add(
            1, std::memory_order_acq_rel) + 1;
    inputState.publicationEpoch = publicationEpoch.fetch_add(
            1, std::memory_order_acq_rel) + 1;
    inputState.revision = 1;
    inputState.anchorRevision = 1;
    inputState.resetRevision = 1;
    stopping = false;
    ensureThread();
    return true;
}

bool WorkoutGameRunner::prepareEngineInput(
        WorkoutGameEngineInput &input,
        std::int64_t workoutTimeMs,
        std::int64_t monotonicTimeMs) const
{
    const bool telemetryStale = expireTelemetry(input, monotonicTimeMs);
    input.simulation.workoutTimeMs = workoutTimeMs;
    if (configuredFeatureLabGapScenario) {
        WorkoutGameFeatureLab::applyGapScenario(
                configuredCourse,
                workoutTimeMs,
                *configuredFeatureLabGapScenario,
                input.simulation);
    }
    return telemetryStale;
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
    std::lock_guard<std::mutex> lifecycleLock(engineLifecycleMutex);
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        inputState.generation = lifecycleGeneration.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        inputState.publicationEpoch = publicationEpoch.fetch_add(
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
    std::lock_guard<std::mutex> lifecycleLock(engineLifecycleMutex);
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        inputState.generation = lifecycleGeneration.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        inputState.publicationEpoch = publicationEpoch.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        inputState.anchorWorkoutTimeMs = std::max<std::int64_t>(0, workoutTimeMs);
        inputState.anchorMonotonicTimeMs = monotonicMilliseconds();
        inputState.rateHint = rateHint;
        inputState.running = true;
        inputState.input.simulation.paused = false;
        ++inputState.anchorRevision;
        ++inputState.resynchronizeRevision;
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
        inputState.publicationEpoch = publicationEpoch.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        inputState.anchorWorkoutTimeMs = std::max<std::int64_t>(0, workoutTimeMs);
        inputState.anchorMonotonicTimeMs = monotonicMilliseconds();
        inputState.rateHint = rateHint;
        ++inputState.anchorRevision;
        ++inputState.revision;
    }
    clearOutput();
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
        const std::int64_t nowMs = monotonicMilliseconds();
        if (inputState.input.telemetryMonotonicTimeMs > nowMs + 1000) {
            inputState.input.telemetryMonotonicTimeMs = nowMs;
        }
        inputState.input.simulation.workoutTimeMs = currentWorkoutTime;
        inputState.input.simulation.paused = paused;
        ++inputState.revision;
    }
    inputChanged.notify_one();
}

void WorkoutGameRunner::pause(std::int64_t workoutTimeMs)
{
    if (!configured) return;
    std::lock_guard<std::mutex> lifecycleLock(engineLifecycleMutex);
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        inputState.generation = lifecycleGeneration.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        inputState.publicationEpoch = publicationEpoch.fetch_add(
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
    WorkoutGameEngineFrame ignored;
    stopAndTakeLatest(workoutTimeMs, ignored);
}

bool WorkoutGameRunner::stopAndTakeLatest(
        std::int64_t workoutTimeMs,
        WorkoutGameEngineFrame &frame)
{
    if (!configured) return false;
    std::lock_guard<std::mutex> lifecycleLock(engineLifecycleMutex);
    {
        std::lock_guard<std::mutex> inputLock(inputMutex);
        inputState.generation = lifecycleGeneration.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        inputState.publicationEpoch = publicationEpoch.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        inputState.anchorWorkoutTimeMs = std::max<std::int64_t>(
                0, workoutTimeMs);
        inputState.anchorMonotonicTimeMs = monotonicMilliseconds();
        inputState.running = false;
        inputState.input.simulation.paused = true;
        ++inputState.anchorRevision;
        ++inputState.revision;
    }
    bool available = false;
    {
        std::lock_guard<std::mutex> outputLock(outputMutex);
        if (latestFrame.sequence != 0
                && latestFrame.sequence != consumedSequence) {
            frame = latestFrame;
            available = true;
        }
        latestFrame = WorkoutGameEngineFrame();
        consumedSequence = publicationSequence;
    }
    inputChanged.notify_one();
    return available;
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
        std::uint64_t expectedGeneration,
        std::uint64_t expectedPublicationEpoch)
{
    std::lock_guard<std::mutex> lock(outputMutex);
    if (expectedGeneration != 0
            && lifecycleGeneration.load(std::memory_order_acquire)
                != expectedGeneration) {
        return false;
    }
    if (expectedPublicationEpoch != 0
            && publicationEpoch.load(std::memory_order_acquire)
                != expectedPublicationEpoch) {
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
    std::uint64_t seenResynchronizeRevision = 0;
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
            std::lock_guard<std::mutex> lifecycleLock(engineLifecycleMutex);
            if (lifecycleGeneration.load(std::memory_order_acquire)
                    != state.generation) {
                continue;
            }
            engine.reset();
            totalSkippedTicks = 0;
            clock.reset(
                    state.anchorWorkoutTimeMs,
                    state.anchorMonotonicTimeMs,
                    state.running,
                    state.rateHint);
            seenResetRevision = state.resetRevision;
            seenResynchronizeRevision = state.resynchronizeRevision;
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

        if (state.resynchronizeRevision != seenResynchronizeRevision) {
            WorkoutGameEngineInput anchorInput = state.input;
            prepareEngineInput(
                    anchorInput,
                    state.anchorWorkoutTimeMs,
                    state.anchorMonotonicTimeMs);
            std::lock_guard<std::mutex> lifecycleLock(engineLifecycleMutex);
            if (lifecycleGeneration.load(std::memory_order_acquire)
                    != state.generation) {
                continue;
            }
            engine.resynchronize(
                    anchorInput,
                    state.anchorMonotonicTimeMs,
                    totalSkippedTicks);
            seenResynchronizeRevision = state.resynchronizeRevision;
        }

        const std::int64_t nowMs = monotonicMilliseconds();
        const WorkoutGameClockAdvance advance = clock.advance(nowMs);
        totalSkippedTicks += advance.skippedTicks;
        std::size_t firstTick = 0;
        if (advance.skippedTicks > 0 && !advance.ticks.empty()) {
            WorkoutGameEngineInput skipInput = state.input;
            prepareEngineInput(
                    skipInput,
                    advance.ticks.front().workoutTimeMs,
                    advance.ticks.front().deadlineMonotonicMs);
            {
                std::lock_guard<std::mutex> lifecycleLock(
                        engineLifecycleMutex);
                if (lifecycleGeneration.load(std::memory_order_acquire)
                        != state.generation) {
                    continue;
                }
                engine.resynchronize(
                        skipInput,
                        advance.ticks.front().deadlineMonotonicMs,
                        totalSkippedTicks);
            }
            firstTick = 1;
        }
        for (std::size_t index = firstTick;
             index < advance.ticks.size(); ++index) {
            const WorkoutGameClockTick &tick = advance.ticks[index];
            WorkoutGameEngineInput tickInput = state.input;
            const bool telemetryStale = prepareEngineInput(
                    tickInput,
                    tick.workoutTimeMs,
                    tick.deadlineMonotonicMs);
            tickInput.simulation.paused = false;
            std::lock_guard<std::mutex> lifecycleLock(engineLifecycleMutex);
            if (lifecycleGeneration.load(std::memory_order_acquire)
                    != state.generation) {
                break;
            }
            WorkoutGameEngineFrame frame = engine.update(
                        tickInput,
                        tick.deadlineMonotonicMs,
                        totalSkippedTicks);
            frame.visual.sessionGeneration = state.generation;
            frame.telemetryStale = telemetryStale;
            if (!publish(
                    std::move(frame),
                    state.generation,
                    state.publicationEpoch)) {
                break;
            }
        }
    }
}

void WorkoutGameRunner::shutdown()
{
    {
        std::lock_guard<std::mutex> lifecycleLock(engineLifecycleMutex);
        lifecycleGeneration.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> inputLock(inputMutex);
        stopping = true;
        ++inputState.revision;
    }
    inputChanged.notify_all();
    if (worker.joinable()) worker.join();
    configured = false;
    stopping = false;
    clearOutput();
}
