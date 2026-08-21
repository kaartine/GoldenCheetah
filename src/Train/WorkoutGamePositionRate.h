/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGamePositionRate_h
#define _GC_WorkoutGamePositionRate_h

#include <cstdint>

struct WorkoutGamePositionRateInput
{
    std::int64_t workoutTimeMs = 0;
    std::int64_t monotonicTimeMs = 0;
    bool motionKnown = false;
    bool moving = false;
};

class WorkoutGamePositionRate
{
public:
    void reset(double initialRate = 1.0);
    double update(const WorkoutGamePositionRateInput &input);
    double rate() const { return currentRate; }

private:
    bool initialized = false;
    std::int64_t lastChangedWorkoutTimeMs = 0;
    std::int64_t lastChangedMonotonicTimeMs = 0;
    double currentRate = 1.0;
    double lastMovingRate = 1.0;
};

#endif
