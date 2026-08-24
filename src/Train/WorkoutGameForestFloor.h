/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameForestFloor_h
#define _GC_WorkoutGameForestFloor_h

#include <algorithm>
#include <cmath>

class WorkoutGameForestFloor
{
public:
    static constexpr double BlendWidthMeters = 3.2;

    static double offsetMeters(
            double worldDistanceMeters,
            double lateralMeters,
            double trailHalfWidthMeters)
    {
        const double side = lateralMeters < 0.0 ? -1.0 : 1.0;
        const double awayFromTrail = std::max(
                0.0, std::abs(lateralMeters) - trailHalfWidthMeters);
        const double blend = std::clamp(
                awayFromTrail / BlendWidthMeters, 0.0, 1.0);
        const double phase = side < 0.0 ? 0.65 : 2.35;
        const double rolling = 0.72
                * std::sin(worldDistanceMeters * 0.052 + phase)
                + 0.34 * std::sin(
                    worldDistanceMeters * 0.137 - phase * 0.6)
                + side * 0.18
                    * std::sin(worldDistanceMeters * 0.031 + 1.2);
        return std::clamp(blend * rolling, -1.05, 1.05);
    }

    static double outerLateralMeters(
            double trailHalfWidthMeters,
            bool left)
    {
        const double lateral = trailHalfWidthMeters + BlendWidthMeters;
        return left ? -lateral : lateral;
    }
};

#endif
