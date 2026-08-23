/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGamePowerProfile_h
#define _GC_WorkoutGamePowerProfile_h

#include "WorkoutGameCourse.h"
#include "WorkoutGameSimulation.h"

#include <vector>

enum class WorkoutGamePowerCueState
{
    None,
    Prepare,
    PushNow,
    Committed,
    Bypassed
};

struct WorkoutGamePowerProfileSegment
{
    double start = 0.0;
    double end = 0.0;
    double targetWatts = 0.0;
    bool challenge = false;
    double challengeStart = 0.0;
    double challengeEnd = 0.0;
    double requiredWatts = 0.0;
};

struct WorkoutGamePowerCue
{
    WorkoutGamePowerCueState state = WorkoutGamePowerCueState::None;
    WorkoutGameChallengeCue challengeCue = WorkoutGameChallengeCue::None;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    double secondsUntilWindow = 0.0;
    double windowProgress = 0.0;
    double requiredWatts = 0.0;
    double actualWatts = 0.0;
    double requiredCadenceRpm = 0.0;
    double actualCadenceRpm = 0.0;
    double requiredSpeedKph = 0.0;
    double maximumSpeedKph = 0.0;
    double actualSpeedKph = 0.0;
    double powerReadiness = 0.0;
    double cadenceReadiness = 0.0;
    double speedReadiness = 0.0;
    double readiness = 0.0;
    bool powerRequired = false;
    bool cadenceRequired = false;
    bool speedRequired = false;
    bool measurementActive = false;
};

struct WorkoutGamePowerProfileSnapshot
{
    bool ready = false;
    double cursor = 0.0;
    double actualWatts = 0.0;
    double targetWatts = 0.0;
    double maximumWatts = 1.0;
    std::vector<WorkoutGamePowerProfileSegment> segments;
    WorkoutGamePowerCue cue;
};

class WorkoutGamePowerProfile
{
public:
    static WorkoutGamePowerProfileSnapshot build(
            const WorkoutGameCourse &course,
            const WorkoutGameSimulationSnapshot &simulation,
            double actualWatts,
            double cadenceRpm = 0.0);
};

#endif
