/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameWorkoutAdapter_h
#define _GC_WorkoutGameWorkoutAdapter_h

#include "WorkoutGameCourse.h"

#include <vector>

struct WorkoutGamePowerPoint
{
    double timeMs = 0.0;
    double watts = 0.0;
};

enum class WorkoutGameWorkoutStatus
{
    Ready,
    EmptyWorkout,
    InvalidPoint
};

struct WorkoutGameWorkout
{
    WorkoutGameWorkoutStatus status = WorkoutGameWorkoutStatus::EmptyWorkout;
    std::vector<WorkoutGameInterval> intervals;
};

class WorkoutGameWorkoutAdapter
{
public:
    static WorkoutGameWorkout normalize(
            const std::vector<WorkoutGamePowerPoint> &points);
};

#endif
