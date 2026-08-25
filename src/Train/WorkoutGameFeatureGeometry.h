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
    Hurdle,
    FacetedLog,
    CurvedTabletop
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

    double surfaceOffset(double localDistanceMeters) const
    {
        if (!ready || localDistanceMeters < startMeters
                || localDistanceMeters > endMeters) {
            return 0.0;
        }
        if (shape == WorkoutGameFeatureGeometryShape::Hurdle) {
            return 0.0;
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
            const auto rampRise = [](double requestedProgress) {
                const double progress = std::clamp(
                        requestedProgress, 0.0, 1.0);
                // Leave at least five sixths of the ramp visibly planar while
                // easing the trail datum into it without a sharp hinge.
                constexpr double Transition = 1.0 / 6.0;
                constexpr double TransitionHeight = Transition * 0.375;
                constexpr double LinearSlope =
                        (1.0 - TransitionHeight) / (1.0 - Transition);
                if (progress >= Transition) {
                    return TransitionHeight
                            + (progress - Transition) * LinearSlope;
                }
                const double amount = progress / Transition;
                const double amount2 = amount * amount;
                const double amount3 = amount2 * amount;
                const double value = (-2.0 * amount3 + 3.0 * amount2)
                        * TransitionHeight
                        + (amount3 - amount2)
                            * Transition * LinearSlope;
                return std::clamp(value, 0.0, TransitionHeight);
            };
            if (localDistanceMeters < plateauStartMeters) {
                const double progress = std::clamp(
                        (localDistanceMeters - startMeters)
                            / (plateauStartMeters - startMeters),
                        0.0, 1.0);
                return heightMeters * rampRise(progress);
            }
            if (localDistanceMeters <= plateauEndMeters) {
                return heightMeters;
            }
            const double progress = std::clamp(
                    (localDistanceMeters - plateauEndMeters)
                        / (endMeters - plateauEndMeters),
                    0.0, 1.0);
            return heightMeters * rampRise(1.0 - progress);
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
        case WorkoutGameTerrainKind::BunnyHop: {
            constexpr double HalfRun = 0.07;
            constexpr double HalfTop = 0.05;
            result = {
                true, -HalfRun, -HalfTop, HalfTop, HalfRun,
                0.10 + 0.10 * difficulty,
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
            constexpr double Pi = 3.14159265358979323846;
            const double height = 0.32 + 0.18 * difficulty;
            // rampRise's eased entry makes its planar normalized slope 1.125.
            // Include that factor so the physical planar ramp remains 15 deg.
            constexpr double PlanarSlopeFactor = 1.125;
            const double rampRun = height * PlanarSlopeFactor
                    / std::tan(15.0 * Pi / 180.0);
            const double deckLength = 1.0 + 0.14 * difficulty;
            const double halfDeck = deckLength * 0.5;
            result = {
                true,
                -halfDeck - rampRun,
                -halfDeck,
                halfDeck,
                halfDeck + rampRun,
                height,
                WorkoutGameFeatureGeometryShape::CurvedTabletop
            };
            break;
        }
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
