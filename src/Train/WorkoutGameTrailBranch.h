/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameTrailBranch_h
#define _GC_WorkoutGameTrailBranch_h

#include <algorithm>
#include <cmath>

class WorkoutGameTrailBranch
{
public:
    static double blend(double requestedProgress)
    {
        const double progress = std::clamp(
                std::isfinite(requestedProgress) ? requestedProgress : 0.0,
                0.0, 1.0);
        if (progress <= 0.0 || progress >= 1.0) return 0.0;
        const auto smoothStep = [](double value) {
            const double clamped = std::clamp(value, 0.0, 1.0);
            return clamped * clamped * (3.0 - 2.0 * clamped);
        };
        if (progress < 0.28) return smoothStep(progress / 0.28);
        if (progress > 0.72) {
            return smoothStep((1.0 - progress) / 0.28);
        }
        return 1.0;
    }

    static double lateralAt(
            double distanceMeters,
            double startDistanceMeters,
            double endDistanceMeters,
            double lateralMeters)
    {
        const double length = endDistanceMeters - startDistanceMeters;
        if (!std::isfinite(distanceMeters)
                || !std::isfinite(startDistanceMeters)
                || !std::isfinite(endDistanceMeters)
                || !std::isfinite(lateralMeters)
                || length <= 0.0) {
            return 0.0;
        }
        return lateralMeters * blend(
                (distanceMeters - startDistanceMeters) / length);
    }
};

#endif
