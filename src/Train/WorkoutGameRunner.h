/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRunner_h
#define _GC_WorkoutGameRunner_h

#include "WorkoutGameClock.h"
#include "WorkoutGameEngine.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

class WorkoutGameRunner
{
public:
    WorkoutGameRunner();
    ~WorkoutGameRunner();

    WorkoutGameRunner(const WorkoutGameRunner &) = delete;
    WorkoutGameRunner &operator=(const WorkoutGameRunner &) = delete;

    bool configure(
            const WorkoutGameCourse &course,
            double ftpWatts,
            bool featureLabEnabled);
    void start(std::int64_t workoutTimeMs, double rateHint);
    void resume(std::int64_t workoutTimeMs, double rateHint);
    void setAnchor(std::int64_t workoutTimeMs, double rateHint);
    void setTelemetry(const WorkoutGameEngineInput &input);
    void pause(std::int64_t workoutTimeMs);
    void stop(std::int64_t workoutTimeMs);
    bool takeLatest(WorkoutGameEngineFrame &frame);
    void shutdown();

    static std::int64_t monotonicMilliseconds();

private:
    struct InputState
    {
        WorkoutGameEngineInput input;
        std::int64_t anchorWorkoutTimeMs = 0;
        std::int64_t anchorMonotonicTimeMs = 0;
        double rateHint = 1.0;
        bool running = false;
        std::uint64_t anchorRevision = 0;
        std::uint64_t resetRevision = 0;
        std::uint64_t revision = 0;
        std::uint64_t generation = 0;
    };

    void ensureThread();
    void run();
    void clearOutput();
    bool publish(
            WorkoutGameEngineFrame frame,
            std::uint64_t expectedGeneration = 0);

    WorkoutGameEngine engine;
    WorkoutGameClock clock;
    WorkoutGameCourse configuredCourse;
    double configuredFtpWatts = 0.0;
    bool configuredFeatureLabEnabled = false;
    std::mutex inputMutex;
    std::condition_variable inputChanged;
    InputState inputState;
    bool configured = false;
    bool stopping = false;
    std::atomic<std::uint64_t> lifecycleGeneration{0};
    std::thread worker;

    std::mutex outputMutex;
    WorkoutGameEngineFrame latestFrame;
    std::uint64_t publicationSequence = 0;
    std::uint64_t consumedSequence = 0;
};

#endif
