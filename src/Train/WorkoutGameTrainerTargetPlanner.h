/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameTrainerTargetPlanner_h
#define _GC_WorkoutGameTrainerTargetPlanner_h

#include "TrainerTargetCoordinator.h"
#include "WorkoutGameCoursePrescription.h"

#include <cstdint>

struct WorkoutGameTrainerTargetInput
{
    WorkoutGameCoursePreset preset = WorkoutGameCoursePreset::Balanced;
    bool targetPowerSupported = false;
    double prescribedWatts = 0.0;
    double gradePercent = 0.0;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    double sectionProgress = 0.0;
    std::int64_t sectionDurationMs = 0;
    double windResistance = 0.0;
    double workoutPosition = 0.0;
};

class WorkoutGameTrainerTargetPlanner
{
public:
    static bool usesTargetPower(
            WorkoutGameCoursePreset preset,
            bool targetPowerSupported);
    static double workoutPowerWatts(
            WorkoutGameCoursePreset preset,
            double prescribedWatts,
            WorkoutGameTerrainKind terrain,
            double sectionProgress,
            std::int64_t sectionDurationMs);
    static double terrainEffortSignal(
            WorkoutGameTerrainKind terrain,
            double sectionProgress,
            std::int64_t sectionDurationMs);
    static TrainerTarget plan(const WorkoutGameTrainerTargetInput &input);
};

#endif
