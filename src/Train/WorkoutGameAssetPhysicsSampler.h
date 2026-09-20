/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameAssetPhysicsSampler_h
#define _GC_WorkoutGameAssetPhysicsSampler_h

#include "WorkoutGameAssetPhysicsSnapshot.h"

#include <cstddef>
#include <vector>

struct WorkoutGameAssetPhysicsSample
{
    bool bound = false;
    bool surfacePresent = false;
    WorkoutGameAssetPhysicsOperation operation =
            WorkoutGameAssetPhysicsOperation::AddObstacle;
    double offsetMeters = 0.0;
    double coulombFriction = 0.0;
    double restitution = 0.0;
};

class WorkoutGameAssetPhysicsSampler
{
public:
    static WorkoutGameAssetPhysicsSample sample(
            const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
            std::size_t pieceIndex,
            double distanceMeters);

    static std::vector<double> breakpointsMeters(
            const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
            std::size_t pieceIndex);
};

#endif
