/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameLegacyFt02V1_h
#define _GC_WorkoutGameLegacyFt02V1_h

#include <algorithm>
#include <cmath>

// Frozen double-precision procedural FT02 contract, before catalog binding.
// Keep operation order, endpoint/tolerance behavior and nonfinite handling.
// This is not the integer catalog evaluator in WorkoutGameFt02PhysicsV1 and
// must not quantize old difficulties or turn decorative logs into colliders.
namespace WorkoutGameLegacyFt02V1 {

constexpr int RadialSegments = 16;

inline double radiusMeters(double requestedDifficulty)
{
    const double difficulty = std::clamp(
            std::isfinite(requestedDifficulty) ? requestedDifficulty : 0.0,
            0.0, 1.0);
    return 0.22 + 0.10 * difficulty;
}

// Bounds and height belong to the caller's profile; do not reconstruct them
// from difficulty. In particular, legacy public profiles can be modified.
// The caller retains its ready/range guard. NaN distance deliberately follows
// the old comparison behavior and reaches the final radius return.
inline double surfaceOffsetMeters(double localDistanceMeters,
                                  double startMeters,
                                  double endMeters,
                                  double heightMeters)
{
    if (localDistanceMeters <= startMeters
            || localDistanceMeters >= endMeters) {
        return 0.0;
    }
    const double radius = heightMeters * 0.5;
    constexpr double Pi = 3.14159265358979323846;
    for (int segment = 0; segment < RadialSegments / 2; ++segment) {
        const double fromAngle = Pi
                - double(segment) * 2.0 * Pi / double(RadialSegments);
        const double toAngle = Pi
                - double(segment + 1) * 2.0 * Pi / double(RadialSegments);
        const double fromX = std::cos(fromAngle) * radius;
        const double toX = std::cos(toAngle) * radius;
        if (localDistanceMeters <= toX + 1e-12) {
            const double amount = std::clamp(
                    (localDistanceMeters - fromX) / (toX - fromX), 0.0, 1.0);
            const double fromY = std::sin(fromAngle) * heightMeters;
            const double toY = std::sin(toAngle) * heightMeters;
            return fromY + (toY - fromY) * amount;
        }
    }
    return radius;
}

} // namespace WorkoutGameLegacyFt02V1

#endif
