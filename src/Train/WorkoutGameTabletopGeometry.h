/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameTabletopGeometry_h
#define _GC_WorkoutGameTabletopGeometry_h

#include <algorithm>
#include <cmath>

struct WorkoutGameTabletopGeometryProfile
{
    bool ready = false;
    double difficulty = 0.0;
    double startMeters = 0.0;
    double lipMeters = 0.0;
    double deckEndMeters = 0.0;
    double endMeters = 0.0;
    double heightMeters = 0.0;
    double socketHalfWidthMeters = 0.0;
    double splitLeadMeters = 0.0;
    double minimumBypassLengthMeters = 0.0;
    double bypassExitRunoutMeters = 0.0;

    double launchSpeedMetersPerSecond(
            double requestedForwardSpeedMetersPerSecond) const
    {
        if (!ready) return 0.0;
        const double forwardSpeed = planningSpeedMetersPerSecond(
                requestedForwardSpeedMetersPerSecond);
        const double duration = flightDurationSeconds(forwardSpeed);
        const double landing = plannedLandingLocalMeters(forwardSpeed);
        const double verticalDisplacement =
                surfaceOffsetMeters(landing) - heightMeters;
        // Compensate for the sprung three-body bike so each difficulty lands
        // on its authored runout instead of using point-mass ballistics.
        const double bikeModelCalibration = 0.55
                - 1.03 * std::min(difficulty, 0.7);
        return std::clamp(
                (verticalDisplacement
                 + 0.5 * GravityMetersPerSecondSquared
                    * duration * duration) / duration
                    + bikeModelCalibration,
                2.0, 5.4);
    }

    bool supportsJumpAtForwardSpeed(
            double requestedForwardSpeedMetersPerSecond) const
    {
        return ready && std::isfinite(requestedForwardSpeedMetersPerSecond)
                && requestedForwardSpeedMetersPerSecond >= 3.0
                && requestedForwardSpeedMetersPerSecond <= 8.0;
    }

    double flightDurationSeconds(
            double requestedForwardSpeedMetersPerSecond) const
    {
        if (!ready) return 0.0;
        const double forwardSpeed = planningSpeedMetersPerSecond(
                requestedForwardSpeedMetersPerSecond);
        return std::clamp(
                (plannedLandingLocalMeters(forwardSpeed) - lipMeters)
                    / forwardSpeed,
                0.45, 2.0);
    }

    double plannedLandingLocalMeters(
            double requestedForwardSpeedMetersPerSecond) const
    {
        if (!ready) return 0.0;
        const double forwardSpeed = planningSpeedMetersPerSecond(
                requestedForwardSpeedMetersPerSecond);
        const double speedProgress = std::clamp(
                (forwardSpeed - 3.0) / 5.0, 0.0, 1.0);
        const double landingProgress = 0.35 + 0.40 * speedProgress;
        return deckEndMeters
                + (endMeters - deckEndMeters) * landingProgress;
    }

    double surfaceOffsetMeters(double localDistanceMeters) const
    {
        if (!ready || !std::isfinite(localDistanceMeters)
                || localDistanceMeters <= startMeters
                || localDistanceMeters >= endMeters) {
            return 0.0;
        }
        if (localDistanceMeters < lipMeters) {
            return heightMeters * rampRise(
                    (localDistanceMeters - startMeters)
                        / (lipMeters - startMeters));
        }
        if (localDistanceMeters <= deckEndMeters) return heightMeters;
        return heightMeters * rampRise(
                1.0 - (localDistanceMeters - deckEndMeters)
                    / (endMeters - deckEndMeters));
    }

private:
    static constexpr double GravityMetersPerSecondSquared = 9.81;

    static double planningSpeedMetersPerSecond(double requestedSpeed)
    {
        return std::clamp(
                std::isfinite(requestedSpeed) ? requestedSpeed : 0.0,
                3.0, 8.0);
    }

    static double rampRise(double requestedProgress)
    {
        const double progress = std::clamp(requestedProgress, 0.0, 1.0);
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
                + (amount3 - amount2) * Transition * LinearSlope;
        return std::clamp(value, 0.0, TransitionHeight);
    }
};

class WorkoutGameTabletopGeometry
{
public:
    static WorkoutGameTabletopGeometryProfile profile(
            double requestedDifficulty)
    {
        const double difficulty = std::clamp(
                std::isfinite(requestedDifficulty)
                    ? requestedDifficulty : 0.0,
                0.0, 1.0);
        constexpr double Pi = 3.14159265358979323846;
        constexpr double PlanarSlopeFactor = 1.125;
        const double height = 0.70 + 0.40 * difficulty;
        const double rampRun = height * PlanarSlopeFactor
                / std::tan((20.0 + 2.0 * difficulty) * Pi / 180.0);
        const double deckLength = 2.4 + 0.80 * difficulty;
        const double halfDeck = deckLength * 0.5;
        const double landingRun = rampRun * 1.15;
        return {
            true,
            difficulty,
            -halfDeck - rampRun,
            -halfDeck,
            halfDeck,
            halfDeck + landingRun,
            height,
            0.68,
            8.0,
            28.0,
            10.0
        };
    }
};

#endif
