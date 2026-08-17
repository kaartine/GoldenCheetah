/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutRideTargetPlanner_h
#define _GC_WorkoutRideTargetPlanner_h

#include <initializer_list>

struct TrainerControlCapabilities
{
    bool targetPower = false;
    bool targetResistance = false;
    bool simulation = false;
    bool nativeVirtualGearing = false;

    static TrainerControlCapabilities targetPowerOnly();
    static TrainerControlCapabilities commonCapabilities(
            std::initializer_list<TrainerControlCapabilities> capabilities);
    void intersectWith(const TrainerControlCapabilities &other);
};

enum class PlannedTrainerTargetMode
{
    StandardErg,
    WorkoutRidePower
};

struct WorkoutRideTargetInput
{
    bool enabled = false;
    double workoutWatts = 0.0;
    double cadenceRpm = 0.0;
    double relativeGearRatio = 1.0;
};

struct PlannedTrainerTarget
{
    PlannedTrainerTargetMode mode = PlannedTrainerTargetMode::StandardErg;
    double targetWatts = 0.0;
};

class WorkoutRideTargetPlanner
{
public:
    static PlannedTrainerTarget plan(
            const WorkoutRideTargetInput &input,
            const TrainerControlCapabilities &capabilities);
};

#endif
