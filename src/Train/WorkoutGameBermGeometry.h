/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameBermGeometry_h
#define _GC_WorkoutGameBermGeometry_h

#include <algorithm>
#include <cmath>

struct WorkoutGameBermGeometryProfile
{
    bool ready = false;
    double startMeters = 0.0;
    double curveStartMeters = 0.0;
    double curveEndMeters = 0.0;
    double endMeters = 0.0;
    double socketHalfWidthMeters = 0.0;
    double activeHalfWidthMeters = 0.0;
    double turnMagnitudeRadians = 0.0;
    double maximumBankRadians = 0.0;

    double headingProgress(double localDistanceMeters) const
    {
        if (!ready || localDistanceMeters <= curveStartMeters) return 0.0;
        if (localDistanceMeters >= curveEndMeters) return 1.0;
        const double progress = std::clamp(
                (localDistanceMeters - curveStartMeters)
                    / (curveEndMeters - curveStartMeters),
                0.0, 1.0);
        const double p2 = progress * progress;
        const double p3 = p2 * progress;
        return p3 * (progress * (progress * 6.0 - 15.0) + 10.0);
    }

    double bankRadians(
            double localDistanceMeters,
            double turnRadians) const
    {
        if (!ready || turnRadians == 0.0
                || localDistanceMeters <= curveStartMeters
                || localDistanceMeters >= curveEndMeters) {
            return 0.0;
        }
        constexpr double Pi = 3.14159265358979323846;
        const double progress = std::clamp(
                (localDistanceMeters - curveStartMeters)
                    / (curveEndMeters - curveStartMeters),
                0.0, 1.0);
        const double envelope = std::pow(std::sin(Pi * progress), 2.0);
        return -std::copysign(maximumBankRadians * envelope, turnRadians);
    }

    double curvatureRadiansPerMeter(
            double localDistanceMeters,
            double turnRadians) const
    {
        if (!ready || turnRadians == 0.0
                || localDistanceMeters <= curveStartMeters
                || localDistanceMeters >= curveEndMeters) {
            return 0.0;
        }
        const double progress = std::clamp(
                (localDistanceMeters - curveStartMeters)
                    / (curveEndMeters - curveStartMeters),
                0.0, 1.0);
        return turnRadians * 30.0 * progress * progress
                * (1.0 - progress) * (1.0 - progress)
                / (curveEndMeters - curveStartMeters);
    }

    double effortLineBias(double actualWatts, double targetWatts) const
    {
        if (!ready || !std::isfinite(actualWatts)
                || !std::isfinite(targetWatts) || targetWatts <= 0.0) {
            return 0.0;
        }
        return std::clamp(
                (actualWatts / targetWatts - 1.0) / 0.40, -1.0, 1.0);
    }

    double effortLineLateralMeters(
            double localDistanceMeters,
            double turnRadians,
            double lineBias) const
    {
        if (!ready || turnRadians == 0.0
                || !std::isfinite(lineBias)
                || localDistanceMeters <= startMeters
                || localDistanceMeters >= endMeters) {
            return 0.0;
        }
        constexpr double Pi = 3.14159265358979323846;
        const double progress = std::clamp(
                (localDistanceMeters - startMeters)
                    / (endMeters - startMeters),
                0.0, 1.0);
        const double outsideDirection = -std::copysign(1.0, turnRadians);
        return outsideDirection * 0.52 * std::clamp(lineBias, -1.0, 1.0)
                * std::pow(std::sin(Pi * progress), 2.0);
    }

    double riderWorldRollRadians(
            double localDistanceMeters,
            double turnRadians,
            double speedMetersPerSecond,
            double lineBias) const
    {
        constexpr double GravityMetersPerSecondSquared = 9.80665;
        constexpr double MaximumRollRadians =
                38.0 * 3.14159265358979323846 / 180.0;
        const double speed = std::max(
                0.0, std::isfinite(speedMetersPerSecond)
                    ? speedMetersPerSecond : 0.0);
        const double curvature = curvatureRadiansPerMeter(
                localDistanceMeters, turnRadians);
        const double ideal = -std::atan(
                speed * speed * curvature
                    / GravityMetersPerSecondSquared);
        const double bias = std::clamp(
                std::isfinite(lineBias) ? lineBias : 0.0, -1.0, 1.0);
        const double lineLeanScale = bias >= 0.0
                ? 1.0 + 0.30 * bias : 1.0 + 0.45 * bias;
        constexpr double CenterLineLeanScale = 0.72;
        return std::clamp(lineLeanScale * CenterLineLeanScale * ideal,
                          -MaximumRollRadians, MaximumRollRadians);
    }

    double halfWidthMeters(double localDistanceMeters) const
    {
        if (!ready || localDistanceMeters <= curveStartMeters
                || localDistanceMeters >= curveEndMeters) {
            return socketHalfWidthMeters;
        }
        constexpr double Pi = 3.14159265358979323846;
        const double progress = std::clamp(
                (localDistanceMeters - curveStartMeters)
                    / (curveEndMeters - curveStartMeters),
                0.0, 1.0);
        const double envelope = std::pow(std::sin(Pi * progress), 2.0);
        return socketHalfWidthMeters
                + (activeHalfWidthMeters - socketHalfWidthMeters) * envelope;
    }

    double surfaceOffsetMeters(
            double localDistanceMeters,
            double lateralMeters,
            double halfWidthMeters,
            double turnRadians) const
    {
        if (!std::isfinite(lateralMeters)
                || !std::isfinite(halfWidthMeters)
                || halfWidthMeters <= 0.0) {
            return 0.0;
        }
        const double bank = bankRadians(localDistanceMeters, turnRadians);
        if (bank == 0.0) return 0.0;
        const double outsideDirection = -std::copysign(1.0, turnRadians);
        const double outsideProgress = std::clamp(
                (outsideDirection * lateralMeters / halfWidthMeters + 1.0)
                    * 0.5,
                0.0, 1.0);
        const double smooth = outsideProgress * outsideProgress
                * (3.0 - 2.0 * outsideProgress);
        const double totalRise = 4.0 * halfWidthMeters / 3.0
                * std::tan(std::abs(bank));
        return totalRise * (smooth - 0.5);
    }
};

class WorkoutGameBermGeometry
{
public:
    static WorkoutGameBermGeometryProfile profile(double requestedDifficulty)
    {
        constexpr double Pi = 3.14159265358979323846;
        const double difficulty = std::clamp(
                std::isfinite(requestedDifficulty)
                    ? requestedDifficulty : 0.0,
                0.0, 1.0);
        const double curveLengthMeters = 6.0 + 4.0 * difficulty;
        const double curveHalfLengthMeters = curveLengthMeters * 0.5;
        constexpr double SocketLengthMeters = 1.5;
        return {
            true,
            -curveHalfLengthMeters - SocketLengthMeters,
            -curveHalfLengthMeters,
            curveHalfLengthMeters,
            curveHalfLengthMeters + SocketLengthMeters,
            0.68 * 1.17,
            0.90 + 0.15 * difficulty,
            (42.0 + 58.0 * difficulty) * Pi / 180.0,
            (18.0 + 16.0 * difficulty) * Pi / 180.0
        };
    }
};

#endif
