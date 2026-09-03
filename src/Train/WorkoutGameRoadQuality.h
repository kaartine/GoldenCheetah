/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRoadQuality_h
#define _GC_WorkoutGameRoadQuality_h

#include "WorkoutGameRoadPlan.h"

#include <vector>

enum class WorkoutGameRoadQualityViolation
{
    NearStraightTooLong,
    RollingWindowTooFewBends,
    RollingWindowDoesNotAlternate,
    RollingWindowTooLittleTurn,
    NearNinetyTurnMissing,
    TurnExceedsBound
};

struct WorkoutGameRoadQualityReport
{
    std::vector<WorkoutGameRoadQualityViolation> violations;

    bool accepted() const { return violations.empty(); }
    bool contains(WorkoutGameRoadQualityViolation violation) const;
};

class WorkoutGameRoadQuality
{
public:
    static constexpr double MaximumNearStraightMeters = 25.0;
    static constexpr double RollingWindowMeters = 100.0;
    static constexpr int MinimumAlternatingBends = 3;
    static constexpr double MinimumAccumulatedTurnDegrees = 45.0;
    static constexpr double DeliberateBendDegrees = 8.0;
    static constexpr double NearNinetyMinimumDegrees = 75.0;
    static constexpr double MaximumTurnDegrees = 85.0;
    static constexpr double MaximumNearNinetySpacingMeters = 400.0;

    static WorkoutGameRoadQualityReport audit(
            const WorkoutGameRoadPlan &plan);
};

#endif
