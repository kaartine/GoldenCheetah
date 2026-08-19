/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameTerrainTransition.h"

#include <algorithm>
#include <cmath>

namespace {

double smoothStep(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    return clamped * clamped * (3.0 - 2.0 * clamped);
}

}

void WorkoutGameTerrainTransition::reset()
{
    initialized = false;
    transitioning = false;
    transitionStartMs = 0;
    current = WorkoutGameWorldSnapshot();
    previous = WorkoutGameTerrainProfile();
}

void WorkoutGameTerrainTransition::setTarget(
        const WorkoutGameWorldSnapshot &world,
        std::int64_t monotonicTimeMs)
{
    if (!world.ready) {
        reset();
        return;
    }
    if (!initialized) {
        initialized = true;
        current = world;
        previous = profile(world);
        return;
    }

    if (isWorldReset(current, world)) {
        current = world;
        previous = profile(world);
        transitioning = false;
        return;
    }
    if (!sameProfile(current, world)) {
        previous = profile(current);
        transitionStartMs = monotonicTimeMs;
        transitioning = true;
    }
    current = world;
}

WorkoutGameTerrainTransitionSnapshot WorkoutGameTerrainTransition::sample(
        std::int64_t monotonicTimeMs) const
{
    WorkoutGameTerrainTransitionSnapshot result;
    result.from = previous;
    if (!initialized || !transitioning) return result;

    result.progress = smoothStep(
            double(std::max<std::int64_t>(
                0, monotonicTimeMs - transitionStartMs))
            / double(DurationMs));
    result.active = result.progress < 1.0;
    return result;
}

WorkoutGameTerrainProfile WorkoutGameTerrainTransition::profile(
        const WorkoutGameWorldSnapshot &world)
{
    WorkoutGameTerrainProfile result;
    result.terrain = world.terrain;
    result.seed = world.seed;
    result.gradePercent = world.gradePercent;
    result.difficulty = world.difficulty;
    result.terrainOffsetMeters = world.terrainOffsetMeters;
    return result;
}

bool WorkoutGameTerrainTransition::sameProfile(
        const WorkoutGameWorldSnapshot &left,
        const WorkoutGameWorldSnapshot &right)
{
    return left.generation == right.generation
            && left.terrain == right.terrain
            && left.seed == right.seed
            && left.gradePercent == right.gradePercent
            && left.difficulty == right.difficulty;
}

bool WorkoutGameTerrainTransition::isWorldReset(
        const WorkoutGameWorldSnapshot &left,
        const WorkoutGameWorldSnapshot &right)
{
    return right.generation != left.generation
            && right.rider.distanceMeters + 2.0
                    < left.rider.distanceMeters;
}
