/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGamePowerCueGeometry_h
#define _GC_WorkoutGamePowerCueGeometry_h

#include "WorkoutGameFeatureRuntime.h"

#include <vector>

struct WorkoutGamePowerCueBand
{
    double startDistanceMeters = 0.0;
    double endDistanceMeters = 0.0;
    double halfWidthRatio = 0.0;
    bool decision = false;
};

class WorkoutGamePowerCueGeometry
{
public:
    static std::vector<WorkoutGamePowerCueBand> build(
            const WorkoutGameFeatureRuntimeSnapshot &feature,
            WorkoutGameChallengeCue cue);
};

#endif
