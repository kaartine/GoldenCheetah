/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameTerrainTransition_h
#define _GC_WorkoutGameTerrainTransition_h

#include "WorkoutGameWorld.h"

#include <cstdint>

struct WorkoutGameTerrainProfile
{
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    std::uint32_t seed = 0;
    double gradePercent = 0.0;
    double difficulty = 0.0;
    double terrainOffsetMeters = 0.0;
};

struct WorkoutGameTerrainTransitionSnapshot
{
    bool active = false;
    WorkoutGameTerrainProfile from;
    double progress = 1.0;
};

class WorkoutGameTerrainTransition
{
public:
    static constexpr std::int64_t DurationMs = 1000;

    void reset();
    void setTarget(
            const WorkoutGameWorldSnapshot &world,
            std::int64_t monotonicTimeMs);
    WorkoutGameTerrainTransitionSnapshot sample(
            std::int64_t monotonicTimeMs) const;

private:
    static WorkoutGameTerrainProfile profile(
            const WorkoutGameWorldSnapshot &world);
    static bool sameProfile(
            const WorkoutGameWorldSnapshot &left,
            const WorkoutGameWorldSnapshot &right);
    static bool isWorldReset(
            const WorkoutGameWorldSnapshot &left,
            const WorkoutGameWorldSnapshot &right);

    bool initialized = false;
    bool transitioning = false;
    std::int64_t transitionStartMs = 0;
    WorkoutGameWorldSnapshot current;
    WorkoutGameTerrainProfile previous;
};

#endif
