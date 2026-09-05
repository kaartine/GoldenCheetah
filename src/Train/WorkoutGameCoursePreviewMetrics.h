/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCoursePreviewMetrics_h
#define _GC_WorkoutGameCoursePreviewMetrics_h

#include "WorkoutGameCourse.h"
#include "WorkoutGameRoadPlan.h"

#include <vector>

struct WorkoutGameCoursePreviewPoint
{
    double progress = 0.0;
    double value = 0.0;
};

struct WorkoutGameCoursePreviewRoadMetrics
{
    int curveEventCount = 0;
    int roadPieceCount = 0;
};

class WorkoutGameCoursePreviewMetrics
{
public:
    static std::vector<WorkoutGameCoursePreviewPoint> workoutPowerProfile(
            const std::vector<WorkoutGameInterval> &intervals);
    static WorkoutGameCoursePreviewRoadMetrics roadMetrics(
            const WorkoutGameRoadPlan &plan);
};

#endif
