/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameReplay_h
#define _GC_WorkoutGameReplay_h

#include "WorkoutGameEngine.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct WorkoutGameReplaySample
{
    WorkoutGameEngineInput input;
    std::int64_t presentationTimeMs = 0;
    std::size_t skippedTicks = 0;
};

struct WorkoutGameReplay
{
    static constexpr std::uint32_t CurrentFormatVersion = 1;

    std::uint32_t formatVersion = CurrentFormatVersion;
    WorkoutGameCourse course;
    double ftpWatts = 0.0;
    bool featureLabEnabled = false;
    std::vector<WorkoutGameReplaySample> samples;
};

enum class WorkoutGameReplayFailure
{
    None,
    UnsupportedFormat,
    InvalidConfiguration,
    EmptyReplay,
    InvalidInput,
    PresentationTimeRegression,
    WorkoutTimeRegression,
    NonFiniteState,
    CourseProgressRegression
};

struct WorkoutGameReplayResult
{
    bool passed = false;
    WorkoutGameReplayFailure failure = WorkoutGameReplayFailure::None;
    std::size_t failureSample = 0;
    std::uint64_t finalStateHash = 0;
    std::vector<std::uint64_t> frameStateHashes;
    WorkoutGameEngineFrame finalFrame;
};

class WorkoutGameReplayHarness
{
public:
    static WorkoutGameReplayResult run(const WorkoutGameReplay &replay);

private:
    static bool validInput(const WorkoutGameReplaySample &sample);
    static bool finiteState(const WorkoutGameEngineFrame &frame);
    static std::uint64_t hashFrame(
            std::uint64_t previousHash,
            const WorkoutGameReplaySample &sample,
            const WorkoutGameEngineFrame &frame);
};

#endif
