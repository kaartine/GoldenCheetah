/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRoadPlan_h
#define _GC_WorkoutGameRoadPlan_h

#include "WorkoutGameAssetPhysicsSnapshot.h"
#include "WorkoutGameRoadCourse.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct WorkoutGameRoadPlan
{
    static constexpr std::uint32_t LegacyGenerationVersion = 1;
    static constexpr std::uint32_t BankAndReliefGenerationVersion = 2;
    static constexpr std::uint32_t CurrentGenerationVersion = 3;
    static constexpr std::size_t MaximumPieces = 4096;

    std::uint32_t generationVersion = CurrentGenerationVersion;
    std::vector<WorkoutGameRoadPiece> pieces;
    std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot>
            assetPhysicsSnapshot;
};

enum class WorkoutGameRoadPlanValidationStatus
{
    Ready,
    UnsupportedVersion,
    ResourceLimit,
    InvalidPlan
};

class WorkoutGameRoadPlanValidator
{
public:
    static WorkoutGameRoadPlanValidationStatus validate(
            const WorkoutGameRoadPlan &plan,
            std::size_t sourceSectionCount);
};

#endif
