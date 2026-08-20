/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameFeatureLab_h
#define _GC_WorkoutGameFeatureLab_h

#include "WorkoutGameCourse.h"
#include "WorkoutGameSimulation.h"

#include <cstdint>

enum class WorkoutGameFeatureLabScenario
{
    Pass,
    Bypass
};

class WorkoutGameFeatureLab
{
public:
    static WorkoutGameCourse course(
            double ftpWatts,
            std::uint32_t seed = 0x464c4142u);
    static double targetWattsAt(
            const WorkoutGameCourse &course,
            std::int64_t workoutTimeMs);
    static WorkoutGameSimulationInput input(
            const WorkoutGameCourse &course,
            std::int64_t workoutTimeMs,
            WorkoutGameFeatureLabScenario scenario);
};

#endif
