/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameWorld.h"

#include <algorithm>
#include <cmath>

namespace {

double finiteOr(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

double approach(double current, double target, double blend)
{
    return current + (target - current) * blend;
}

double targetYaw(WorkoutGameCameraMode mode)
{
    switch (mode) {
    case WorkoutGameCameraMode::Side: return 90.0;
    case WorkoutGameCameraMode::ThreeQuarter: return 42.0;
    case WorkoutGameCameraMode::Chase: return 8.0;
    }
    return 90.0;
}

double targetZoom(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::Drop:
        return 0.9;
    case WorkoutGameTerrainKind::Skinny:
    case WorkoutGameTerrainKind::Berm:
        return 1.08;
    default:
        return 1.0;
    }
}

}

WorkoutGameCameraMode WorkoutGameCamera::preferredMode(
        WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::Roots:
    case WorkoutGameTerrainKind::Rollers:
    case WorkoutGameTerrainKind::RockGarden:
        return WorkoutGameCameraMode::ThreeQuarter;
    case WorkoutGameTerrainKind::Skinny:
    case WorkoutGameTerrainKind::Berm:
        return WorkoutGameCameraMode::Chase;
    default:
        return WorkoutGameCameraMode::Side;
    }
}

void WorkoutGameCamera::reset()
{
    initialized = false;
    currentGeneration = 0;
    current = WorkoutGameCameraSnapshot();
}

WorkoutGameCameraSnapshot WorkoutGameCamera::update(
        const WorkoutGameWorldSnapshot &world,
        double elapsedSeconds)
{
    if (!world.ready) {
        reset();
        return current;
    }

    const WorkoutGameCameraMode mode = preferredMode(world.terrain);
    const double distance = finiteOr(world.rider.distanceMeters, 0.0);
    const double elevation = finiteOr(world.rider.elevationMeters, 0.0);
    const double speed = std::clamp(
            finiteOr(world.speedMetersPerSecond, 0.0), 0.0, 30.0);
    const double lookAhead = std::clamp(3.0 + speed * 0.55, 3.0, 15.0);
    const double pitch = std::clamp(
            finiteOr(world.rider.pitchDegrees, 0.0) * 0.3,
            -12.0, 12.0);

    if (!initialized || world.generation != currentGeneration) {
        current.ready = true;
        current.mode = mode;
        current.centerDistanceMeters = distance;
        current.centerElevationMeters = elevation + 1.2;
        current.lookAheadMeters = lookAhead;
        current.zoom = targetZoom(world.terrain);
        current.yawDegrees = targetYaw(mode);
        current.pitchDegrees = pitch;
        currentGeneration = world.generation;
        initialized = true;
        return current;
    }

    const double dt = std::clamp(
            finiteOr(elapsedSeconds, 0.0), 0.0, 0.1);
    const double followBlend = 1.0 - std::exp(-dt / 0.18);
    const double angleBlend = 1.0 - std::exp(-dt / 0.45);
    current.ready = true;
    current.mode = mode;
    current.centerDistanceMeters = approach(
            current.centerDistanceMeters, distance, followBlend);
    current.centerElevationMeters = approach(
            current.centerElevationMeters, elevation + 1.2, followBlend);
    current.lookAheadMeters = approach(
            current.lookAheadMeters, lookAhead, followBlend);
    current.zoom = approach(
            current.zoom, targetZoom(world.terrain), angleBlend);
    current.yawDegrees = approach(
            current.yawDegrees, targetYaw(mode), angleBlend);
    current.pitchDegrees = approach(
            current.pitchDegrees, pitch, angleBlend);
    return current;
}
