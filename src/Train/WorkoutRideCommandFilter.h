/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutRideCommandFilter_h
#define _GC_WorkoutRideCommandFilter_h

#include <cstdint>

struct WorkoutRideCommandDecision
{
    bool dispatch = false;
    bool hasEffectiveTarget = false;
    double effectiveWatts = 0.0;
    int retryAfterMs = -1;
};

class WorkoutRideCommandFilter
{
public:
    WorkoutRideCommandDecision update(
            double requestedWatts,
            double baseWorkoutWatts,
            std::int64_t nowMs);
    void reset();

private:
    bool initialized = false;
    double lastSentWatts = 0.0;
    double lastBaseWorkoutWatts = 0.0;
    std::int64_t lastDispatchMs = 0;
};

#endif
