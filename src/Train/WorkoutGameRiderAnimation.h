/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRiderAnimation_h
#define _GC_WorkoutGameRiderAnimation_h

#include <algorithm>
#include <cmath>

struct WorkoutGameRiderAnimationInput
{
    double watts = 0.0;
    double targetWatts = 0.0;
    double gradePercent = 0.0;
    double cadenceRpm = 0.0;
    bool walking = false;
    bool airborne = false;
};

struct WorkoutGameRiderAnimationTarget
{
    double standingBlend = 0.0;
    double pedalEffortBlend = 0.0;
};

class WorkoutGameRiderAnimation
{
public:
    static WorkoutGameRiderAnimationTarget target(
            const WorkoutGameRiderAnimationInput &input)
    {
        WorkoutGameRiderAnimationTarget result;
        if (!std::isfinite(input.watts)
                || !std::isfinite(input.targetWatts)
                || !std::isfinite(input.gradePercent)
                || !std::isfinite(input.cadenceRpm)
                || input.walking || input.airborne
                || input.watts <= 0.0 || input.cadenceRpm < 15.0) {
            return result;
        }

        const double effortRatio = input.targetWatts > 1.0
                ? input.watts / input.targetWatts : 0.0;
        const double relativeEffort = smootherStep(
                (effortRatio - 0.68) / 0.52);
        const double absoluteEffort = smootherStep(
                (input.watts - 110.0) / 250.0);
        result.pedalEffortBlend = std::clamp(
                0.58 * relativeEffort + 0.42 * absoluteEffort,
                0.0, 1.0);

        const double climb = smootherStep(
                (input.gradePercent - 2.5) / 6.5);
        const double hardPedaling = smootherStep(
                (input.watts - 165.0) / 175.0);
        const double lowCadence = 1.0 - smootherStep(
                (input.cadenceRpm - 58.0) / 30.0);
        result.standingBlend = std::clamp(
                climb * hardPedaling * (0.68 + 0.32 * lowCadence),
                0.0, 1.0);
        return result;
    }

private:
    static double smootherStep(double value)
    {
        const double t = std::clamp(value, 0.0, 1.0);
        return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    }
};

#endif
