/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameAssetPhysicsResolver_h
#define _GC_WorkoutGameAssetPhysicsResolver_h

#include "WorkoutGameAssetPhysicsSnapshot.h"
#include "WorkoutGameRoadCourse.h"

#include <memory>
#include <vector>

class WorkoutGameAssetCatalog;

enum class WorkoutGameAssetPhysicsResolveStatus
{
    Ready,
    UnsupportedCatalog,
    MissingAsset,
    MissingProfile,
    InvalidAnchor,
    InvalidDifficulty,
    ResourceLimit
};

struct WorkoutGameAssetPhysicsResolution
{
    WorkoutGameAssetPhysicsResolveStatus status =
            WorkoutGameAssetPhysicsResolveStatus::UnsupportedCatalog;
    std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot> snapshot;
};

class WorkoutGameAssetPhysicsResolver
{
public:
    static WorkoutGameAssetPhysicsResolution resolve(
            const WorkoutGameAssetCatalog &catalog,
            const std::vector<WorkoutGameRoadPiece> &pieces);
};

#endif
