/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameSkinnyGeometry_h
#define _GC_WorkoutGameSkinnyGeometry_h

#include <algorithm>
#include <cmath>

struct WorkoutGameSkinnyGeometryProfile
{
    bool ready = false;
    double startMeters = 0.0;
    double activeStartMeters = 0.0;
    double deckStartMeters = 0.0;
    double deckEndMeters = 0.0;
    double activeEndMeters = 0.0;
    double endMeters = 0.0;
    double socketHalfWidthMeters = 0.0;
    double deckHalfWidthMeters = 0.0;
    double deckHeightMeters = 0.0;
    double deckThicknessMeters = 0.0;

    double halfWidthMeters(double) const
    {
        return socketHalfWidthMeters;
    }

    double deckSurfaceOffsetMeters(double localDistanceMeters) const
    {
        if (!ready || !std::isfinite(localDistanceMeters)
                || localDistanceMeters <= activeStartMeters
                || localDistanceMeters >= activeEndMeters) {
            return 0.0;
        }
        if (localDistanceMeters < deckStartMeters) {
            return deckHeightMeters * std::clamp(
                    (localDistanceMeters - activeStartMeters)
                        / (deckStartMeters - activeStartMeters), 0.0, 1.0);
        }
        if (localDistanceMeters <= deckEndMeters) return deckHeightMeters;
        return deckHeightMeters * std::clamp(
                (activeEndMeters - localDistanceMeters)
                    / (activeEndMeters - deckEndMeters), 0.0, 1.0);
    }

    double surfaceOffsetMeters(
            double localDistanceMeters,
            double lateralMeters) const
    {
        if (!ready || !std::isfinite(lateralMeters)
                || std::abs(lateralMeters) > deckHalfWidthMeters) {
            return 0.0;
        }
        return deckSurfaceOffsetMeters(localDistanceMeters);
    }

    double balanceRollDegrees(double localDistanceMeters) const
    {
        if (!ready || !std::isfinite(localDistanceMeters)
                || localDistanceMeters <= deckStartMeters
                || localDistanceMeters >= deckEndMeters) {
            return 0.0;
        }
        constexpr double Pi = 3.14159265358979323846;
        const double progress = (localDistanceMeters - deckStartMeters)
                / (deckEndMeters - deckStartMeters);
        const double envelope = std::sin(Pi * progress);
        const double difficulty = std::clamp(
                (0.31 - deckHalfWidthMeters) / 0.06, 0.0, 1.0);
        return (1.0 + difficulty) * envelope
                * std::sin(4.0 * Pi * progress + 0.35);
    }
};

class WorkoutGameSkinnyGeometry
{
public:
    static WorkoutGameSkinnyGeometryProfile profile(
            double requestedDifficulty)
    {
        const double difficulty = std::clamp(
                std::isfinite(requestedDifficulty)
                    ? requestedDifficulty : 0.0,
                0.0, 1.0);
        WorkoutGameSkinnyGeometryProfile result;
        result.ready = true;
        result.startMeters = -14.0;
        result.activeStartMeters = -12.0;
        result.deckStartMeters = -8.0;
        result.deckEndMeters = 8.0;
        result.activeEndMeters = 12.0;
        result.endMeters = 14.0;
        result.socketHalfWidthMeters = 0.68;
        result.deckHalfWidthMeters = 0.31 - 0.06 * difficulty;
        result.deckHeightMeters = 0.28 + 0.08 * difficulty;
        result.deckThicknessMeters = 0.06;
        return result;
    }
};

#endif
