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
#include "WorkoutGameTabletopGeometry.h"

#include <algorithm>
#include <cmath>

enum class WorkoutGameFeatureGeometryShape
{
    Linear,
    Hurdle,
    FacetedLog,
    CurvedTabletop,
    DropLedge,
    RoundedRollers
};

constexpr int WorkoutGameLogRadialSegments = 16;

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
    double landingStartMeters = 0.0;
    double recoveryStartMeters = 0.0;
    double difficulty = 0.0;

    bool surfacePresent(double localDistanceMeters) const
    {
        if (!ready || localDistanceMeters < startMeters
                || localDistanceMeters > endMeters) {
            return true;
        }
        return shape != WorkoutGameFeatureGeometryShape::DropLedge
                || localDistanceMeters <= plateauStartMeters
                || localDistanceMeters >= landingStartMeters;
    }

    double surfaceOffset(double localDistanceMeters) const
    {
        if (!ready || localDistanceMeters < startMeters
                || localDistanceMeters > endMeters) {
            return 0.0;
        }
        if (shape == WorkoutGameFeatureGeometryShape::Hurdle) {
            return 0.0;
        }
        if (shape == WorkoutGameFeatureGeometryShape::DropLedge) {
            if (localDistanceMeters <= plateauStartMeters) return 0.0;
            if (localDistanceMeters <= recoveryStartMeters) {
                return heightMeters;
            }
            const double progress = std::clamp(
                    (localDistanceMeters - recoveryStartMeters)
                        / (endMeters - recoveryStartMeters),
                    0.0, 1.0);
            const double smooth = progress * progress
                    * (3.0 - 2.0 * progress);
            return heightMeters * (1.0 - smooth);
        }
        if (shape == WorkoutGameFeatureGeometryShape::RoundedRollers) {
            if (localDistanceMeters < plateauStartMeters
                    || localDistanceMeters > plateauEndMeters) {
                return 0.0;
            }
            constexpr double Pi = 3.14159265358979323846;
            const double coreProgress = std::clamp(
                    (localDistanceMeters - plateauStartMeters)
                        / (plateauEndMeters - plateauStartMeters),
                    0.0, 1.0);
            return heightMeters * 0.5 * (
                    1.0 - std::cos(6.0 * Pi * coreProgress));
        }
        if (shape == WorkoutGameFeatureGeometryShape::FacetedLog) {
            if (localDistanceMeters <= startMeters
                    || localDistanceMeters >= endMeters) {
                return 0.0;
            }
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
                    const double fromY =
                            std::sin(fromAngle) * heightMeters;
                    const double toY =
                            std::sin(toAngle) * heightMeters;
                    return fromY + (toY - fromY) * amount;
                }
            }
            return radius;
        }
        if (shape == WorkoutGameFeatureGeometryShape::CurvedTabletop) {
            return WorkoutGameTabletopGeometry::profile(
                    difficulty).surfaceOffsetMeters(localDistanceMeters);
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
        case WorkoutGameTerrainKind::Rollers:
            result = {
                true, -5.25, -4.5, 4.5, 5.25,
                0.20 + 0.08 * difficulty,
                WorkoutGameFeatureGeometryShape::RoundedRollers
            };
            break;
        case WorkoutGameTerrainKind::BunnyHop: {
            constexpr double HalfRun = 0.11;
            constexpr double HalfTop = 0.075;
            result = {
                true, -HalfRun, -HalfTop, HalfTop, HalfRun,
                0.18 + 0.14 * difficulty,
                WorkoutGameFeatureGeometryShape::Hurdle
            };
            break;
        }
        case WorkoutGameTerrainKind::LogOver: {
            const double radius = 0.22 + 0.10 * difficulty;
            result = {
                true, -radius, 0.0, 0.0, radius, 2.0 * radius,
                WorkoutGameFeatureGeometryShape::FacetedLog
            };
            break;
        }
        case WorkoutGameTerrainKind::Tabletop: {
            const WorkoutGameTabletopGeometryProfile tabletop =
                    WorkoutGameTabletopGeometry::profile(difficulty);
            result = {
                true,
                tabletop.startMeters,
                tabletop.lipMeters,
                tabletop.deckEndMeters,
                tabletop.endMeters,
                tabletop.heightMeters,
                WorkoutGameFeatureGeometryShape::CurvedTabletop
            };
            result.difficulty = difficulty;
            break;
        }
        case WorkoutGameTerrainKind::Drop:
            result = {true, -10.0, 0.0, 1.8 + difficulty, 14.0,
                      -(0.60 + 0.40 * difficulty),
                      WorkoutGameFeatureGeometryShape::DropLedge,
                      1.8 + difficulty, 6.0};
            break;
        default:
            break;
        }
        return result;
    }
};

#endif
