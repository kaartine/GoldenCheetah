/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameHorizon_h
#define _GC_WorkoutGameHorizon_h

#include <cstdint>
#include <vector>

struct WorkoutGameHorizonSnapshot
{
    bool ready = false;
    std::vector<double> farRidgeY;
    std::vector<double> nearRidgeY;
};

class WorkoutGameHorizon
{
public:
    static WorkoutGameHorizonSnapshot build(
            std::uint32_t seed,
            double riderDistanceMeters,
            int sampleCount = 32);
};

#endif
