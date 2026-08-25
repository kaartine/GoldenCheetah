/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRockSlabGeometry_h
#define _GC_WorkoutGameRockSlabGeometry_h

#include <algorithm>
#include <cmath>

struct WorkoutGameRockSlabGeometryProfile
{
    bool ready = false;
    double startMeters = 0.0;
    double activeStartMeters = 0.0;
    double crestMeters = 0.0;
    double activeEndMeters = 0.0;
    double endMeters = 0.0;
    double socketHalfWidthMeters = 0.0;
    double activeHalfWidthMeters = 0.0;
    double safeLineLateralMeters = 0.0;
    double heightMeters = 0.0;
    double sideDepthMeters = 0.0;

    double safeLineOffsetMeters(double localDistanceMeters) const
    {
        if (!ready || localDistanceMeters <= startMeters
                || localDistanceMeters >= endMeters) {
            return 0.0;
        }
        if (localDistanceMeters < activeStartMeters) {
            return safeLineLateralMeters * smoothStep(
                    (localDistanceMeters - startMeters)
                        / (activeStartMeters - startMeters));
        }
        if (localDistanceMeters <= activeEndMeters) {
            return safeLineLateralMeters;
        }
        return safeLineLateralMeters * smoothStep(
                (endMeters - localDistanceMeters)
                    / (endMeters - activeEndMeters));
    }

    double halfWidthMeters(double localDistanceMeters) const
    {
        if (!ready || safeLineLateralMeters <= 0.0) {
            return socketHalfWidthMeters;
        }
        const double envelope = safeLineOffsetMeters(localDistanceMeters)
                / safeLineLateralMeters;
        return socketHalfWidthMeters
                + (activeHalfWidthMeters - socketHalfWidthMeters) * envelope;
    }

    double slabCenterLateralMeters(double localDistanceMeters) const
    {
        if (!ready || localDistanceMeters <= activeStartMeters
                || localDistanceMeters >= activeEndMeters) {
            return 0.0;
        }
        const double progress = (localDistanceMeters - activeStartMeters)
                / (activeEndMeters - activeStartMeters);
        constexpr double Pi = 3.14159265358979323846;
        const double envelope = std::sin(Pi * progress);
        return envelope * (-0.31
                + 0.06 * std::sin(2.0 * Pi * progress + 0.35));
    }

    double slabHalfWidthMeters(double localDistanceMeters) const
    {
        if (!ready || localDistanceMeters < activeStartMeters
                || localDistanceMeters > activeEndMeters) {
            return 0.0;
        }
        const double progress = (localDistanceMeters - activeStartMeters)
                / (activeEndMeters - activeStartMeters);
        constexpr double Pi = 3.14159265358979323846;
        return std::clamp(
                0.78 + 0.08 * std::sin(Pi * progress)
                    + 0.04 * std::sin(5.0 * Pi * progress + 0.6),
                0.72, 0.88);
    }

    double longitudinalHeightMeters(double localDistanceMeters) const
    {
        if (!ready || !std::isfinite(localDistanceMeters)
                || localDistanceMeters <= activeStartMeters
                || localDistanceMeters >= activeEndMeters) {
            return 0.0;
        }
        if (localDistanceMeters < crestMeters) {
            return heightMeters * smoothStep(
                    (localDistanceMeters - activeStartMeters)
                        / (crestMeters - activeStartMeters));
        }
        return heightMeters * (1.0 - smoothStep(
                (localDistanceMeters - crestMeters)
                    / (activeEndMeters - crestMeters)));
    }

    double surfaceOffsetMeters(
            double localDistanceMeters,
            double lateralMeters) const
    {
        if (!ready || !std::isfinite(lateralMeters)) return 0.0;
        const double halfWidth = slabHalfWidthMeters(localDistanceMeters);
        if (halfWidth <= 0.0) return 0.0;
        const double normalized = (lateralMeters
                - slabCenterLateralMeters(localDistanceMeters)) / halfWidth;
        if (std::abs(normalized) > 1.0) return 0.0;
        const double crossfall = std::clamp(
                1.0 - 0.035 * normalized, 0.965, 1.035);
        return longitudinalHeightMeters(localDistanceMeters) * crossfall;
    }

private:
    static double smoothStep(double value)
    {
        const double p = std::clamp(value, 0.0, 1.0);
        return p * p * (3.0 - 2.0 * p);
    }
};

class WorkoutGameRockSlabGeometry
{
public:
    static WorkoutGameRockSlabGeometryProfile profile(
            double requestedDifficulty)
    {
        const double difficulty = std::clamp(
                std::isfinite(requestedDifficulty)
                    ? requestedDifficulty : 0.0,
                0.0, 1.0);
        WorkoutGameRockSlabGeometryProfile result;
        result.ready = true;
        result.startMeters = -7.0;
        result.activeStartMeters = -3.8;
        result.crestMeters = 0.25;
        result.activeEndMeters = 3.6;
        result.endMeters = 7.0;
        result.socketHalfWidthMeters = 0.68;
        result.activeHalfWidthMeters = 1.42;
        result.safeLineLateralMeters = 1.05;
        result.heightMeters = 0.62 + 0.34 * difficulty;
        result.sideDepthMeters = 0.16 + 0.06 * difficulty;
        return result;
    }
};

#endif
