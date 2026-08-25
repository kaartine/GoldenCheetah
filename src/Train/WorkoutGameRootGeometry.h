/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRootGeometry_h
#define _GC_WorkoutGameRootGeometry_h

#include <algorithm>
#include <array>
#include <cmath>

struct WorkoutGameRootSegment
{
    double startForwardMeters = 0.0;
    double startLateralMeters = 0.0;
    double endForwardMeters = 0.0;
    double endLateralMeters = 0.0;
    double startRadiusMeters = 0.0;
    double endRadiusMeters = 0.0;
};

struct WorkoutGameRootGeometryProfile
{
    bool ready = false;
    double startMeters = 0.0;
    double activeStartMeters = 0.0;
    double activeEndMeters = 0.0;
    double endMeters = 0.0;
    double socketHalfWidthMeters = 0.68;
    double activeHalfWidthMeters = 1.25;
    double safeLineLateralMeters = 0.82;
    double burialRatio = 0.07;
    std::array<WorkoutGameRootSegment, 8> segments = {};

    double safeLineOffsetMeters(double localDistanceMeters) const
    {
        if (!ready || localDistanceMeters <= -5.0
                || localDistanceMeters >= 5.0) {
            return 0.0;
        }
        const auto smootherStep = [](double value) {
            const double p = std::clamp(value, 0.0, 1.0);
            return p * p * p * (p * (p * 6.0 - 15.0) + 10.0);
        };
        if (localDistanceMeters < activeStartMeters) {
            return safeLineLateralMeters * smootherStep(
                    (localDistanceMeters + 5.0)
                    / (activeStartMeters + 5.0));
        }
        if (localDistanceMeters <= activeEndMeters) {
            return safeLineLateralMeters;
        }
        return safeLineLateralMeters * smootherStep(
                (5.0 - localDistanceMeters)
                / (5.0 - activeEndMeters));
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

    double surfaceOffsetMeters(
            double localDistanceMeters,
            double lateralMeters) const
    {
        if (!ready || !std::isfinite(localDistanceMeters)
                || !std::isfinite(lateralMeters)
                || localDistanceMeters <= activeStartMeters
                || localDistanceMeters >= activeEndMeters) {
            return 0.0;
        }
        double height = 0.0;
        for (const WorkoutGameRootSegment &root : segments) {
            const double forward = root.endForwardMeters
                    - root.startForwardMeters;
            const double lateral = root.endLateralMeters
                    - root.startLateralMeters;
            const double lengthSquared = forward * forward
                    + lateral * lateral;
            if (lengthSquared <= 1e-12) continue;
            const double progress = std::clamp(
                    ((localDistanceMeters - root.startForwardMeters) * forward
                     + (lateralMeters - root.startLateralMeters) * lateral)
                        / lengthSquared,
                    0.0, 1.0);
            const double closestForward = root.startForwardMeters
                    + progress * forward;
            const double closestLateral = root.startLateralMeters
                    + progress * lateral;
            const double distance = std::hypot(
                    localDistanceMeters - closestForward,
                    lateralMeters - closestLateral);
            const double radius = root.startRadiusMeters
                    + progress * (root.endRadiusMeters
                                  - root.startRadiusMeters);
            if (distance >= radius || radius <= 0.0) continue;
            const double crown = std::sqrt(std::max(
                    0.0, radius * radius - distance * distance));
            height = std::max(height, crown - burialRatio * radius);
        }
        return std::max(0.0, height);
    }
};

class WorkoutGameRootGeometry
{
public:
    static WorkoutGameRootGeometryProfile profile(double requestedDifficulty)
    {
        const double difficulty = std::clamp(
                std::isfinite(requestedDifficulty)
                    ? requestedDifficulty : 0.0,
                0.0, 1.0);
        const double scale = 0.86 + 0.22 * difficulty;
        const auto root = [scale](
                double startForward,
                double startLateral,
                double endForward,
                double endLateral,
                double startRadius,
                double endRadius) {
            return WorkoutGameRootSegment{
                startForward, startLateral, endForward, endLateral,
                startRadius * scale, endRadius * scale
            };
        };
        WorkoutGameRootGeometryProfile result;
        result.ready = true;
        result.startMeters = -6.0;
        result.activeStartMeters = -2.0;
        result.activeEndMeters = 2.0;
        result.endMeters = 6.0;
        result.segments = {{
            root(-1.95, -1.38, -1.68, -0.52, 0.125, 0.105),
            root(-1.68, -0.52, -1.34,  0.55, 0.105, 0.030),
            root(-1.68, -0.52, -0.72,  0.50, 0.095, 0.028),
            root(-0.38, -1.30, -0.04,  0.55, 0.120, 0.030),
            root( 0.02,  1.35,  0.30,  0.96, 0.115, 0.045),
            root( 0.02,  0.46,  0.92, -0.98, 0.092, 0.040),
            root( 0.70, -1.28,  1.10,  0.50, 0.105, 0.030),
            root( 1.48, -1.30,  1.78,  0.48, 0.092, 0.026)
        }};
        return result;
    }
};

#endif
