/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameFeatureGeometry_h
#define _GC_WorkoutGameFeatureGeometry_h

#include "WorkoutGameWorld.h"

#include <algorithm>
#include <cmath>

enum class WorkoutGameFeatureGeometryShape
{
    Linear,
    FacetedLog
};

constexpr int WorkoutGameLogRadialSegments = 8;

struct WorkoutGameFeatureGeometryProfile
{
    bool ready = false;
    double startMeters = 0.0;
    double plateauStartMeters = 0.0;
    double plateauEndMeters = 0.0;
    double endMeters = 0.0;
    double heightMeters = 0.0;
    WorkoutGameFeatureGeometryShape shape =
            WorkoutGameFeatureGeometryShape::Linear;

    double surfaceOffset(double localDistanceMeters) const
    {
        if (!ready || localDistanceMeters < startMeters
                || localDistanceMeters > endMeters) {
            return 0.0;
        }
        if (shape == WorkoutGameFeatureGeometryShape::FacetedLog) {
            const double radius = heightMeters * 0.5;
            constexpr double Pi = 3.14159265358979323846;
            for (int segment = 0;
                 segment < WorkoutGameLogRadialSegments / 2; ++segment) {
                const double fromAngle = Pi
                        - double(segment) * 2.0 * Pi
                            / double(WorkoutGameLogRadialSegments);
                const double toAngle = Pi
                        - double(segment + 1) * 2.0 * Pi
                            / double(WorkoutGameLogRadialSegments);
                const double fromX = std::cos(fromAngle) * radius;
                const double toX = std::cos(toAngle) * radius;
                if (localDistanceMeters <= toX + 1e-12) {
                    const double amount = std::clamp(
                            (localDistanceMeters - fromX) / (toX - fromX),
                            0.0, 1.0);
                    const double fromY = radius
                            + std::sin(fromAngle) * radius;
                    const double toY = radius
                            + std::sin(toAngle) * radius;
                    return fromY + (toY - fromY) * amount;
                }
            }
            return radius;
        }
        if (localDistanceMeters < plateauStartMeters) {
            return heightMeters * (
                    (localDistanceMeters - startMeters)
                    / (plateauStartMeters - startMeters));
        }
        if (localDistanceMeters <= plateauEndMeters) return heightMeters;
        return heightMeters * (1.0 - (
                (localDistanceMeters - plateauEndMeters)
                / (endMeters - plateauEndMeters)));
    }
};

class WorkoutGameFeatureGeometry
{
public:
    static WorkoutGameFeatureGeometryProfile profile(
            WorkoutGameTerrainKind terrain,
            double requestedDifficulty)
    {
        const double difficulty = std::clamp(
                std::isfinite(requestedDifficulty)
                    ? requestedDifficulty : 0.0,
                0.0, 1.0);
        WorkoutGameFeatureGeometryProfile result;
        switch (terrain) {
        case WorkoutGameTerrainKind::BunnyHop:
        case WorkoutGameTerrainKind::LogOver: {
            const double radius = 0.22 + 0.10 * difficulty;
            result = {
                true, -radius, 0.0, 0.0, radius, 2.0 * radius,
                WorkoutGameFeatureGeometryShape::FacetedLog
            };
            break;
        }
        case WorkoutGameTerrainKind::Tabletop:
            result = {true, -3.2, -1.25, 1.25, 3.2,
                      0.65 + 0.35 * difficulty};
            break;
        case WorkoutGameTerrainKind::RockSlab:
            result = {true, -3.0, 0.0, 3.0, 6.0,
                      0.30 + 0.25 * difficulty};
            break;
        case WorkoutGameTerrainKind::Drop:
            result = {true, 0.0, 2.0, 6.0, 10.0,
                      -(0.7 + 0.5 * difficulty)};
            break;
        default:
            break;
        }
        return result;
    }
};

#endif
