/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGame3DCameraComfort_h
#define _GC_WorkoutGame3DCameraComfort_h

#include "WorkoutGameRoadCourse.h"

#include <cstdint>

struct WorkoutGame3DCameraComfortInput
{
    std::int64_t workoutTimeMs = 0;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    bool mainLine = false;
    double surfaceOffsetMeters = 0.0;
};

class WorkoutGame3DCameraComfort
{
public:
    static constexpr double MaximumRootBumpMeters = 0.022;
    static constexpr double MaximumVerticalSpeedMetersPerSecond = 0.18;

    static double supportElevationMeters(
            const WorkoutGameRoadSample &sample);

    void reset();
    double update(const WorkoutGame3DCameraComfortInput &input);
    double verticalOffsetMeters() const { return currentOffsetMeters; }

private:
    bool initialized = false;
    std::int64_t lastWorkoutTimeMs = 0;
    double currentOffsetMeters = 0.0;
};

#endif
