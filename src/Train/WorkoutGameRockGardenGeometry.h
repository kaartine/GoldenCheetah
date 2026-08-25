/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRockGardenGeometry_h
#define _GC_WorkoutGameRockGardenGeometry_h

#include <algorithm>
#include <array>
#include <cmath>

struct WorkoutGameRockGardenStone
{
    double forwardMeters = 0.0;
    double lateralMeters = 0.0;
    double forwardRadiusMeters = 0.0;
    double lateralRadiusMeters = 0.0;
    double heightMeters = 0.0;
    double yawRadians = 0.0;
};

struct WorkoutGameRockGardenGeometryProfile
{
    bool ready = false;
    double startMeters = 0.0;
    double activeStartMeters = 0.0;
    double activeEndMeters = 0.0;
    double endMeters = 0.0;
    double socketHalfWidthMeters = 0.68;
    double activeHalfWidthMeters = 0.68;
    double safeLineLateralMeters = 0.0;
    double burialRatio = 0.0;
    std::array<WorkoutGameRockGardenStone, 12> stones = {};

    double safeLineOffsetMeters(double localDistanceMeters) const
    {
        if (!ready || localDistanceMeters <= -6.0
                || localDistanceMeters >= 6.0) {
            return 0.0;
        }
        const auto smootherStep = [](double value) {
            const double p = std::clamp(value, 0.0, 1.0);
            return p * p * (3.0 - 2.0 * p);
        };
        if (localDistanceMeters < activeStartMeters) {
            return safeLineLateralMeters * smootherStep(
                    (localDistanceMeters + 6.0)
                    / (activeStartMeters + 6.0));
        }
        if (localDistanceMeters <= activeEndMeters) {
            return safeLineLateralMeters;
        }
        return safeLineLateralMeters * smootherStep(
                (6.0 - localDistanceMeters)
                / (6.0 - activeEndMeters));
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
        for (const WorkoutGameRockGardenStone &stone : stones) {
            const double forward = localDistanceMeters
                    - stone.forwardMeters;
            const double lateral = lateralMeters - stone.lateralMeters;
            const double cosine = std::cos(stone.yawRadians);
            const double sine = std::sin(stone.yawRadians);
            const double stoneForward = forward * cosine + lateral * sine;
            const double stoneLateral = -forward * sine + lateral * cosine;
            const double normalized =
                    stoneForward * stoneForward
                        / (stone.forwardRadiusMeters
                           * stone.forwardRadiusMeters)
                    + stoneLateral * stoneLateral
                        / (stone.lateralRadiusMeters
                           * stone.lateralRadiusMeters);
            if (normalized >= 1.0) continue;
            const double visibleHeight =
                    (1.0 - burialRatio) * stone.heightMeters;
            height = std::max(
                    height,
                    visibleHeight * std::sqrt(std::max(
                        0.0, 1.0 - normalized)));
        }
        return height;
    }
};

class WorkoutGameRockGardenGeometry
{
public:
    static WorkoutGameRockGardenGeometryProfile profile(
            double requestedDifficulty)
    {
        const double difficulty = std::clamp(
                std::isfinite(requestedDifficulty)
                    ? requestedDifficulty : 0.0,
                0.0, 1.0);
        const double scale = 0.88 + 0.38 * difficulty;
        const auto stone = [scale](
                double forward,
                double lateral,
                double forwardRadius,
                double lateralRadius,
                double height,
                double yaw) {
            return WorkoutGameRockGardenStone{
                forward, lateral,
                forwardRadius, lateralRadius,
                height * scale, yaw
            };
        };
        WorkoutGameRockGardenGeometryProfile result;
        result.ready = true;
        result.startMeters = -7.0;
        result.activeStartMeters = -3.25;
        result.activeEndMeters = 3.25;
        result.endMeters = 7.0;
        result.socketHalfWidthMeters = 0.68;
        result.activeHalfWidthMeters = 1.35;
        result.safeLineLateralMeters = 0.88;
        result.burialRatio = 0.18;
        result.stones = {{
            stone(-2.75, -0.14, 0.42, 0.48, 0.15,  0.15),
            stone(-2.15,  0.18, 0.34, 0.46, 0.18, -0.22),
            stone(-1.52, -0.10, 0.48, 0.52, 0.16,  0.31),
            stone(-0.84,  0.15, 0.38, 0.47, 0.22, -0.18),
            stone(-0.17, -0.18, 0.46, 0.50, 0.17,  0.24),
            stone( 0.52,  0.14, 0.35, 0.46, 0.20, -0.29),
            stone( 1.14, -0.12, 0.44, 0.53, 0.16,  0.17),
            stone( 1.78,  0.19, 0.40, 0.48, 0.19, -0.25),
            stone( 2.52, -0.14, 0.46, 0.50, 0.15,  0.28),
            stone(-1.92, -0.92, 0.38, 0.34, 0.14,  0.42),
            stone(-0.18, -1.00, 0.46, 0.38, 0.17, -0.36),
            stone( 1.62, -0.94, 0.40, 0.35, 0.15,  0.33)
        }};
        return result;
    }
};

#endif
