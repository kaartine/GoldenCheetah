/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameGapJumpSelector_h
#define _GC_WorkoutGameGapJumpSelector_h

#include "WorkoutGameGapJumpGeometry.h"

#include <cstdint>

struct WorkoutGameGapJumpSelectionState
{
    WorkoutGameGapJumpLine provisionalLine = WorkoutGameGapJumpLine::None;
    WorkoutGameGapJumpLine lockedLine = WorkoutGameGapJumpLine::None;
    bool locked = false;
    std::uint64_t actionId = 0;
};

class WorkoutGameGapJumpSelector
{
public:
    WorkoutGameGapJumpSelector();
    explicit WorkoutGameGapJumpSelector(
            const WorkoutGameGapJumpGeometryProfile &profile);

    void reset();
    WorkoutGameGapJumpSelectionState update(
            double filteredSpeedMetersPerSecond,
            int durationMilliseconds);
    WorkoutGameGapJumpSelectionState lock(
            std::uint64_t actionId,
            bool effortReady,
            bool telemetryFresh);
    WorkoutGameGapJumpSelectionState state() const;

    static WorkoutGameGapJumpLine coldSelection(
            double filteredSpeedMetersPerSecond);
    static WorkoutGameGapJumpLine coldSelection(
            const WorkoutGameGapJumpGeometryProfile &profile,
            double filteredSpeedMetersPerSecond);

private:
    static constexpr int PromotionDurationMilliseconds = 300;
    static constexpr int DemotionDurationMilliseconds = 500;
    static constexpr int MaximumInputDurationMilliseconds = 250;

    WorkoutGameGapJumpLine schmittCandidate(double speed) const;
    void clearCandidate();

    WorkoutGameGapJumpGeometryProfile profile_;
    WorkoutGameGapJumpSelectionState state_;
    WorkoutGameGapJumpLine candidate_ = WorkoutGameGapJumpLine::None;
    int candidateDurationMilliseconds_ = 0;
    bool initialized_ = false;
};

#endif
