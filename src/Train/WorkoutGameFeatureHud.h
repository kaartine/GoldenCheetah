/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameFeatureHud_h
#define _GC_WorkoutGameFeatureHud_h

#include "WorkoutGameFeatureRuntime.h"

enum class WorkoutGameFeatureHudState
{
    Hidden,
    Prepare,
    Measure,
    Committed,
    ActNow,
    Complete,
    Bypass,
    NoBonus,
    Launch
};

enum class WorkoutGameFeatureHudDistanceKind
{
    None,
    Decision,
    Action,
    Launch
};

struct WorkoutGameFeatureHudSnapshot
{
    bool visible = false;
    WorkoutGameFeatureHudState state = WorkoutGameFeatureHudState::Hidden;
    WorkoutGameFeatureHudDistanceKind distanceKind =
            WorkoutGameFeatureHudDistanceKind::None;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    WorkoutGameRoute route = WorkoutGameRoute::MainLine;
    double distanceMeters = 0.0;
    bool powerRequired = false;
    double requiredPowerWatts = 0.0;
    int powerReadinessPercent = 0;
    bool cadenceRequired = false;
    double requiredCadenceRpm = 0.0;
    int cadenceReadinessPercent = 0;
};

class WorkoutGameFeatureHud
{
public:
    static WorkoutGameFeatureHudSnapshot build(
            const WorkoutGameFeatureRuntimeSnapshot &feature,
            const WorkoutGameSimulationSnapshot &simulation,
            double targetWatts);
};

#endif
