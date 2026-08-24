/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRiderVisual_h
#define _GC_WorkoutGameRiderVisual_h

#include "WorkoutGameFeatureRuntime.h"
#include "WorkoutGameWorld.h"

struct WorkoutGameRiderVisualPose
{
    bool airborne = false;
    double airHeightMeters = 0.0;
    double liftPixels = 0.0;
    double shadowScale = 1.0;
    double shadowOpacity = 0.42;
    double riderWidthScale = 1.0;
    double riderHeightScale = 1.0;
    double screenRollDegrees = 0.0;
};

class WorkoutGameRiderVisual
{
public:
    static WorkoutGameRiderVisualPose pose(
            const WorkoutGameWorldSnapshot &world,
            const WorkoutGameFeatureRuntimeSnapshot &feature,
            double riderHeightPixels);
};

#endif
