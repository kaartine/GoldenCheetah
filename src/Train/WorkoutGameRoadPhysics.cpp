/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameRoadPhysics.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double GravityMetersPerSecondSquared = 9.80665;
constexpr std::int64_t IntegrationStepMs = 10;
constexpr std::int64_t MaximumUpdateTimeMs = 10000;
constexpr double MaximumPowerWatts = 3000.0;
constexpr double MaximumGradePercent = 30.0;

bool finitePositive(double value)
{
    return std::isfinite(value) && value > 0.0;
}

bool finiteNonNegative(double value)
{
    return std::isfinite(value) && value >= 0.0;
}

double sanitized(double value, double lower, double upper)
{
    return std::isfinite(value) ? std::clamp(value, lower, upper) : lower;
}

}

bool WorkoutGameRoadPhysics::validParameters(
        const WorkoutGameRoadPhysicsParameters &parameters)
{
    return finitePositive(parameters.totalMassKg)
            && finiteNonNegative(parameters.dragAreaSquareMeters)
            && finiteNonNegative(parameters.rollingResistanceCoefficient)
            && finitePositive(parameters.airDensityKgPerCubicMeter)
            && finitePositive(parameters.drivetrainEfficiency)
            && parameters.drivetrainEfficiency <= 1.0
            && finitePositive(parameters.rotatingMassFactor)
            && finitePositive(parameters.lowSpeedMetersPerSecond)
            && finitePositive(parameters.maximumDriveForceNewtons)
            && finitePositive(parameters.maximumBrakeForceNewtons)
            && finitePositive(parameters.maximumSpeedMetersPerSecond);
}

bool WorkoutGameRoadPhysics::configure(
        const WorkoutGameRoadPhysicsParameters &parameters)
{
    if (!validParameters(parameters)) {
        configured = false;
        configuredParameters = WorkoutGameRoadPhysicsParameters();
        reset();
        return false;
    }

    configuredParameters = parameters;
    configured = true;
    reset();
    return true;
}

void WorkoutGameRoadPhysics::reset(double initialSpeed)
{
    elapsedTimeMs = 0;
    droppedUpdateTimeMs = 0;
    distanceMeters = 0.0;
    elevationMeters = 0.0;
    speedMetersPerSecond = configured
            ? sanitized(
                initialSpeed, 0.0,
                configuredParameters.maximumSpeedMetersPerSecond)
            : 0.0;
}

void WorkoutGameRoadPhysics::integrateStep(
        const WorkoutGameRoadPhysicsInput &input,
        double elapsedSeconds)
{
    const double grade = input.gradePercent / 100.0;
    const double angle = std::atan(grade);
    const double mass = configuredParameters.totalMassKg;
    const double effectiveMass = mass * configuredParameters.rotatingMassFactor;
    const double oldSpeed = speedMetersPerSecond;

    const double wheelPower = input.powerWatts
            * configuredParameters.drivetrainEfficiency;
    const double driveForce = std::min(
            configuredParameters.maximumDriveForceNewtons,
            wheelPower / std::max(
                oldSpeed, configuredParameters.lowSpeedMetersPerSecond));
    const double gradeForce = mass * GravityMetersPerSecondSquared
            * std::sin(angle);
    const double rollingForce = mass * GravityMetersPerSecondSquared
            * configuredParameters.rollingResistanceCoefficient
            * std::cos(angle);
    const double aerodynamicForce = 0.5
            * configuredParameters.airDensityKgPerCubicMeter
            * configuredParameters.dragAreaSquareMeters
            * oldSpeed * oldSpeed;
    const double brakeForce = input.brake
            * configuredParameters.maximumBrakeForceNewtons;
    const double netForce = driveForce - gradeForce - rollingForce
            - aerodynamicForce - brakeForce;

    speedMetersPerSecond = std::clamp(
            oldSpeed + netForce / effectiveMass * elapsedSeconds,
            0.0, configuredParameters.maximumSpeedMetersPerSecond);
    const double distanceStep = (oldSpeed + speedMetersPerSecond)
            * 0.5 * elapsedSeconds;
    distanceMeters += distanceStep;
    elevationMeters += distanceStep * std::sin(angle);
}

WorkoutGameRoadPhysicsSnapshot WorkoutGameRoadPhysics::snapshot() const
{
    WorkoutGameRoadPhysicsSnapshot result;
    result.ready = configured;
    result.elapsedTimeMs = elapsedTimeMs;
    result.droppedUpdateTimeMs = droppedUpdateTimeMs;
    result.speedMetersPerSecond = speedMetersPerSecond;
    result.distanceMeters = distanceMeters;
    result.elevationMeters = elevationMeters;
    return result;
}

WorkoutGameRoadPhysicsSnapshot WorkoutGameRoadPhysics::update(
        const WorkoutGameRoadPhysicsInput &rawInput,
        std::int64_t requestedTimeMs)
{
    if (!configured) return WorkoutGameRoadPhysicsSnapshot();

    WorkoutGameRoadPhysicsInput input;
    input.powerWatts = sanitized(rawInput.powerWatts, 0.0, MaximumPowerWatts);
    input.gradePercent = sanitized(
            rawInput.gradePercent,
            -MaximumGradePercent, MaximumGradePercent);
    input.brake = sanitized(rawInput.brake, 0.0, 1.0);

    const std::int64_t nonNegativeTimeMs = std::max<std::int64_t>(
            0, requestedTimeMs);
    const std::int64_t acceptedTimeMs = std::min(
            nonNegativeTimeMs, MaximumUpdateTimeMs);
    droppedUpdateTimeMs += nonNegativeTimeMs - acceptedTimeMs;
    elapsedTimeMs += acceptedTimeMs;
    std::int64_t remainingTimeMs = acceptedTimeMs;
    while (remainingTimeMs > 0) {
        const std::int64_t stepMs = std::min(
                remainingTimeMs, IntegrationStepMs);
        integrateStep(input, double(stepMs) / 1000.0);
        remainingTimeMs -= stepMs;
    }
    return snapshot();
}
