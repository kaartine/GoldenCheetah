/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCourse_h
#define _GC_WorkoutGameCourse_h

#include <cstdint>
#include <vector>

enum class WorkoutGameFeature
{
    WarmupTrail,
    Trail,
    FlowTrail,
    Climb,
    SprintJump,
    RecoveryDescent,
    CooldownDescent
};

struct WorkoutGameInterval
{
    std::int64_t startMs = 0;
    std::int64_t durationMs = 0;
    double startWatts = 0.0;
    double endWatts = 0.0;
};

struct WorkoutGameSection
{
    WorkoutGameFeature feature = WorkoutGameFeature::Trail;
    std::int64_t startMs = 0;
    std::int64_t durationMs = 0;
    double targetWatts = 0.0;
    double gradePercent = 0.0;
    double difficulty = 0.0;
    int challengeCount = 0;
    std::uint32_t visualVariant = 0;
    bool gravityAssisted = false;
};

enum class WorkoutGameCourseStatus
{
    Ready,
    EmptyWorkout,
    InvalidFtp,
    InvalidInterval
};

struct WorkoutGameCourse
{
    WorkoutGameCourseStatus status = WorkoutGameCourseStatus::EmptyWorkout;
    std::uint32_t seed = 0;
    std::int64_t durationMs = 0;
    std::vector<WorkoutGameSection> sections;
};

class WorkoutGameCourseBuilder
{
public:
    static WorkoutGameCourse build(
            const std::vector<WorkoutGameInterval> &intervals,
            double ftpWatts,
            std::uint32_t requestedSeed = 0);
};

#endif
