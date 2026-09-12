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

#include "WorkoutGameWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

enum class WorkoutGameRiderMotionPhase
{
    Pedal,
    Preload,
    Airborne,
    Landing,
    RoughSurface
};

struct WorkoutGameRiderMotionInput
{
    double elapsedSeconds = 0.0;
    WorkoutGameRiderMotionPhase phase = WorkoutGameRiderMotionPhase::Pedal;
    double phaseProgress = 0.0;
    double landingImpact = 0.0;
    double rearSuspensionCompression = 0.0;
    double frontSuspensionCompression = 0.0;
    double roughSurfacePumpMeters =
            std::numeric_limits<double>::quiet_NaN();
};

struct WorkoutGameRiderMotionState
{
    double pumpMeters = 0.0;
    double rearSuspensionCompression = 0.0;
    double frontSuspensionCompression = 0.0;
};

struct WorkoutGameTailwhipInput
{
    bool airborne = false;
    bool jumpMotion = false;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    double flightProgress = 0.0;
    double speedKph = 0.0;
    std::uint64_t actionId = 0;
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

    static WorkoutGameRiderMotionState advanceMotion(
            const WorkoutGameRiderMotionState &previous,
            const WorkoutGameRiderMotionInput &input)
    {
        WorkoutGameRiderMotionState result {
            finiteOr(previous.pumpMeters, 0.0),
            finiteOr(previous.rearSuspensionCompression, 0.0),
            finiteOr(previous.frontSuspensionCompression, 0.0)
        };
        result.pumpMeters = std::clamp(result.pumpMeters, -0.12, 0.06);
        result.rearSuspensionCompression = std::clamp(
                result.rearSuspensionCompression, 0.0, 1.0);
        result.frontSuspensionCompression = std::clamp(
                result.frontSuspensionCompression, 0.0, 1.0);

        if (!std::isfinite(input.elapsedSeconds)
                || input.elapsedSeconds <= 0.0) {
            return result;
        }
        const double elapsedSeconds = std::clamp(
                input.elapsedSeconds, 0.0, 0.10);
        const double rearTarget = std::clamp(finiteOr(
                input.rearSuspensionCompression, 0.0), 0.0, 1.0);
        const double frontTarget = std::clamp(finiteOr(
                input.frontSuspensionCompression, 0.0), 0.0, 1.0);
        result.rearSuspensionCompression = filteredStep(
                result.rearSuspensionCompression, rearTarget,
                elapsedSeconds, rearTarget > result.rearSuspensionCompression
                    ? 0.045 : 0.075,
                6.0, 0.0, 1.0);
        result.frontSuspensionCompression = filteredStep(
                result.frontSuspensionCompression, frontTarget,
                elapsedSeconds, frontTarget > result.frontSuspensionCompression
                    ? 0.045 : 0.075,
                6.0, 0.0, 1.0);

        const double progress = std::clamp(
                finiteOr(input.phaseProgress, 0.0), 0.0, 1.0);
        const double landingImpact = std::clamp(
                finiteOr(input.landingImpact, 0.0), 0.0, 1.0);
        const double meanCompression = 0.5 * (
                result.rearSuspensionCompression
                + result.frontSuspensionCompression);
        const double splitCompression = std::abs(
                result.rearSuspensionCompression
                - result.frontSuspensionCompression);

        double pumpTarget = 0.0;
        double pumpTimeConstant = 0.11;
        switch (input.phase) {
        case WorkoutGameRiderMotionPhase::Preload:
            pumpTarget = -0.08 * smootherStep(progress);
            pumpTimeConstant = 0.075;
            break;
        case WorkoutGameRiderMotionPhase::Airborne:
            pumpTarget = 0.05 * std::sin(
                    3.14159265358979323846 * progress);
            pumpTimeConstant = 0.095;
            break;
        case WorkoutGameRiderMotionPhase::Landing:
            pumpTarget = -0.105 * landingImpact
                    - 0.015 * meanCompression;
            pumpTimeConstant = 0.055;
            break;
        case WorkoutGameRiderMotionPhase::RoughSurface:
            pumpTarget = std::clamp(finiteOr(
                    input.roughSurfacePumpMeters,
                    -0.055 * (0.65 * meanCompression
                        + 0.35 * splitCompression)), -0.10, 0.06);
            pumpTimeConstant = 0.065;
            break;
        case WorkoutGameRiderMotionPhase::Pedal:
            break;
        }
        result.pumpMeters = filteredStep(
                result.pumpMeters, pumpTarget, elapsedSeconds,
                pumpTimeConstant, 0.65, -0.12, 0.06);
        return result;
    }

    static double tailwhipDegrees(const WorkoutGameTailwhipInput &input)
    {
        if (!input.airborne || !input.jumpMotion
                || !std::isfinite(input.flightProgress)
                || !std::isfinite(input.speedKph)
                || input.flightProgress <= 0.0
                || input.flightProgress >= 1.0) {
            return 0.0;
        }
        double featureScale = 0.0;
        switch (input.terrain) {
        case WorkoutGameTerrainKind::BunnyHop: featureScale = 0.55; break;
        case WorkoutGameTerrainKind::LogOver: featureScale = 0.68; break;
        case WorkoutGameTerrainKind::Tabletop:
        case WorkoutGameTerrainKind::GapJump:
            featureScale = 1.0;
            break;
        default: return 0.0;
        }
        const double speedBlend = smootherStep(
                (input.speedKph - 12.0) / 28.0);
        const double peakDegrees = featureScale
                * (14.0 + 42.0 * speedBlend);
        const double envelope = std::pow(
                std::sin(3.14159265358979323846
                         * input.flightProgress), 1.15);
        const double direction = (input.actionId & 1u) == 0u ? -1.0 : 1.0;
        return direction * peakDegrees * envelope;
    }

private:
    static double finiteOr(double value, double fallback)
    {
        return std::isfinite(value) ? value : fallback;
    }

    static double filteredStep(double current, double target,
                               double elapsedSeconds,
                               double timeConstant,
                               double maximumRate,
                               double minimum,
                               double maximum)
    {
        const double blend = 1.0 - std::exp(
                -elapsedSeconds / std::max(1.0e-6, timeConstant));
        const double maximumStep = maximumRate * elapsedSeconds;
        const double step = std::clamp(
                (target - current) * blend, -maximumStep, maximumStep);
        return std::clamp(current + step, minimum, maximum);
    }

    static double smootherStep(double value)
    {
        const double t = std::clamp(value, 0.0, 1.0);
        return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    }
};

#endif
