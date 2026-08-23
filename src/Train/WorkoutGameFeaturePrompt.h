/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameFeaturePrompt_h
#define _GC_WorkoutGameFeaturePrompt_h

#include "WorkoutGameFeatureRuntime.h"
#include "WorkoutGamePowerProfile.h"

enum class WorkoutGameFeatureInstruction
{
    RideSteady,
    GetReady,
    PedalHard,
    CarrySpeed,
    HoldLine,
    KeepClimbing,
    Ready,
    FeatureCommitted,
    Takeoff,
    Absorb,
    Drop,
    SafeLine
};

struct WorkoutGameFeaturePromptSnapshot
{
    WorkoutGameFeatureInstruction instruction =
            WorkoutGameFeatureInstruction::RideSteady;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    double distanceMeters = 0.0;
    bool featureActive = false;
};

class WorkoutGameFeaturePrompt
{
public:
    static WorkoutGameFeaturePromptSnapshot build(
            const WorkoutGamePowerProfileSnapshot &profile,
            const WorkoutGameFeatureRuntimeSnapshot &feature);
};

#endif
