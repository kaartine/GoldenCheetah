/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameClock_h
#define _GC_WorkoutGameClock_h

#include <cstddef>
#include <cstdint>
#include <vector>

struct WorkoutGameClockTick
{
    std::int64_t deadlineMonotonicMs = 0;
    std::int64_t workoutTimeMs = 0;
};

struct WorkoutGameClockAdvance
{
    std::vector<WorkoutGameClockTick> ticks;
    std::size_t skippedTicks = 0;
};

class WorkoutGameClock
{
public:
    explicit WorkoutGameClock(
            std::int64_t stepMs = 20,
            std::size_t maximumCatchupTicks = 4);

    void reset(
            std::int64_t workoutTimeMs,
            std::int64_t monotonicTimeMs,
            bool running,
            double rate);
    void setAnchor(
            std::int64_t workoutTimeMs,
            std::int64_t monotonicTimeMs,
            bool running,
            double rateHint);
    void setRunning(bool running, std::int64_t monotonicTimeMs);
    std::int64_t positionAt(std::int64_t monotonicTimeMs) const;
    WorkoutGameClockAdvance advance(std::int64_t monotonicTimeMs);

    static std::int64_t monotonicMilliseconds();

    std::int64_t stepMilliseconds() const { return stepMs; }
    std::int64_t nextDeadlineMonotonicMilliseconds() const
    {
        return nextDeadlineMonotonicMs;
    }
    bool isRunning() const { return running; }

private:
    std::int64_t nextDeadlineAfter(std::int64_t monotonicTimeMs) const;

    std::int64_t stepMs;
    std::size_t maximumCatchupTicks;
    bool initialized = false;
    bool running = false;
    std::int64_t anchorWorkoutTimeMs = 0;
    std::int64_t anchorMonotonicTimeMs = 0;
    std::int64_t nextDeadlineMonotonicMs = 0;
    std::int64_t lastSourceWorkoutTimeMs = 0;
    std::int64_t lastPublishedWorkoutTimeMs = 0;
    double timelineRate = 1.0;
};

#endif
