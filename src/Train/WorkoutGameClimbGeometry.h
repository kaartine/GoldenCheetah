/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameClimbGeometry_h
#define _GC_WorkoutGameClimbGeometry_h

#include <algorithm>
#include <array>
#include <cmath>

struct WorkoutGameClimbStep
{
    double forwardMeters = 0.0;
    double lateralMeters = 0.0;
    double halfLengthMeters = 0.0;
    double halfWidthMeters = 0.0;
    double heightMeters = 0.0;
    double yawDegrees = 0.0;

    bool operator==(const WorkoutGameClimbStep &other) const
    {
        return forwardMeters == other.forwardMeters
                && lateralMeters == other.lateralMeters
                && halfLengthMeters == other.halfLengthMeters
                && halfWidthMeters == other.halfWidthMeters
                && heightMeters == other.heightMeters
                && yawDegrees == other.yawDegrees;
    }
};

struct WorkoutGameClimbGeometryProfile
{
    bool ready = false;
    double startMeters = 0.0;
    double activeStartMeters = 0.0;
    double crestStartMeters = 0.0;
    double endMeters = 0.0;
    double socketHalfWidthMeters = 0.0;
    double minimumLengthMeters = 0.0;
    double entryTransitionMeters = 0.0;
    double crestTransitionMeters = 0.0;
    double contactRampMeters = 0.0;
    std::array<WorkoutGameClimbStep, 5> steps = {};

    double crestRelease(double localDistanceMeters) const
    {
        if (!ready || !std::isfinite(localDistanceMeters)) return 0.0;
        if (localDistanceMeters <= crestStartMeters) return 1.0;
        if (localDistanceMeters >= endMeters) return 0.0;
        const double progress = (localDistanceMeters - crestStartMeters)
                / (endMeters - crestStartMeters);
        return 1.0 - smootherStep(progress);
    }

    double standingBlend(
            double effortRatio,
            double cadenceRpm,
            bool walking) const
    {
        if (!ready || walking || !std::isfinite(effortRatio)
                || !std::isfinite(cadenceRpm)) {
            return 0.0;
        }
        const double effort = smootherStep((effortRatio - 0.82) / 0.28);
        const double lowCadence = 1.0
                - smootherStep((cadenceRpm - 58.0) / 24.0);
        return std::clamp(effort * (0.72 + 0.28 * lowCadence), 0.0, 1.0);
    }

    double surfaceOffsetMeters(
            double localDistanceMeters,
            double lateralMeters = 0.0) const
    {
        if (!ready || !std::isfinite(localDistanceMeters)
                || !std::isfinite(lateralMeters)) {
            return 0.0;
        }
        double result = 0.0;
        for (const WorkoutGameClimbStep &step : steps) {
            const double yawRadians = step.yawDegrees
                    * 3.14159265358979323846 / 180.0;
            const double cosine = std::cos(yawRadians);
            const double sine = std::sin(yawRadians);
            const double forwardDelta = localDistanceMeters
                    - step.forwardMeters;
            const double lateralDelta = lateralMeters
                    - step.lateralMeters;
            const double stepForward = forwardDelta * cosine
                    + lateralDelta * sine;
            const double stepLateral = -forwardDelta * sine
                    + lateralDelta * cosine;
            result = std::max(result, stepSurfaceOffsetMeters(
                    step, stepForward, stepLateral));
        }
        return result;
    }

    double stepSurfaceOffsetMeters(
            const WorkoutGameClimbStep &step,
            double stepForwardMeters,
            double stepLateralMeters) const
    {
        if (!ready || !std::isfinite(stepForwardMeters)
                || !std::isfinite(stepLateralMeters)
                || std::abs(stepLateralMeters)
                    > step.halfWidthMeters + 1e-9) {
            return 0.0;
        }
        const double distance = std::abs(stepForwardMeters);
        if (distance <= step.halfLengthMeters + 1e-9) {
            return step.heightMeters;
        }
        if (distance >= step.halfLengthMeters + contactRampMeters) {
            return 0.0;
        }
        const double ramp = 1.0 - (distance - step.halfLengthMeters)
                / contactRampMeters;
        return step.heightMeters * smootherStep(ramp);
    }

    double sustainedGradePercent(
            double sectionLengthMeters,
            double requestedAverageGradePercent,
            double entryGradePercent,
            double exitGradePercent,
            double entryLengthMeters,
            double crestLengthMeters) const
    {
        if (!ready || !std::isfinite(sectionLengthMeters)
                || !std::isfinite(requestedAverageGradePercent)
                || !std::isfinite(entryGradePercent)
                || !std::isfinite(exitGradePercent)
                || !std::isfinite(entryLengthMeters)
                || !std::isfinite(crestLengthMeters)) {
            return 0.0;
        }
        const double sustainedLength = std::max(
                0.0, sectionLengthMeters - entryLengthMeters
                    - crestLengthMeters);
        const double denominator = 0.5 * entryLengthMeters
                + sustainedLength + 0.5 * crestLengthMeters;
        if (denominator <= 0.0) return requestedAverageGradePercent;
        return (requestedAverageGradePercent * sectionLengthMeters
                - 0.5 * entryGradePercent * entryLengthMeters
                - 0.5 * exitGradePercent * crestLengthMeters)
                / denominator;
    }

private:
    static double smootherStep(double value)
    {
        const double p = std::clamp(value, 0.0, 1.0);
        return p * p * p * (p * (p * 6.0 - 15.0) + 10.0);
    }
};

class WorkoutGameClimbGeometry
{
public:
    static WorkoutGameClimbGeometryProfile profile(double requestedDifficulty)
    {
        const double difficulty = std::clamp(
                std::isfinite(requestedDifficulty)
                    ? requestedDifficulty : 0.0,
                0.0, 1.0);
        const auto step = [difficulty](
                double forward, double lateral, double halfLength,
                double halfWidth, double baseHeight, double yaw) {
            return WorkoutGameClimbStep{
                forward,
                lateral,
                halfLength * (0.94 + 0.10 * difficulty),
                halfWidth,
                baseHeight * (0.86 + 0.28 * difficulty),
                yaw
            };
        };

        WorkoutGameClimbGeometryProfile result;
        result.ready = true;
        result.startMeters = -14.0;
        result.activeStartMeters = -11.5;
        result.crestStartMeters = -5.0;
        result.endMeters = 0.0;
        result.socketHalfWidthMeters = 0.68;
        result.minimumLengthMeters = result.endMeters - result.startMeters;
        result.entryTransitionMeters = 5.0;
        result.crestTransitionMeters = 5.0;
        result.contactRampMeters = 0.24;
        result.steps = {{
            step(-10.1, -0.06, 0.17, 0.60, 0.10,  3.0),
            step( -8.5,  0.04, 0.20, 0.56, 0.13, -4.0),
            step( -7.1, -0.02, 0.16, 0.62, 0.09,  2.0),
            step( -5.9,  0.06, 0.21, 0.54, 0.14, -3.0),
            step( -5.15,-0.04, 0.15, 0.61, 0.08,  4.0)
        }};
        return result;
    }
};

#endif
