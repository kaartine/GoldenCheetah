/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRoadPhysics_h
#define _GC_WorkoutGameRoadPhysics_h

#include <cstdint>

struct WorkoutGameRoadPhysicsParameters
{
    double totalMassKg = 92.0;
    double dragAreaSquareMeters = 0.5;
    double rollingResistanceCoefficient = 0.012;
    double airDensityKgPerCubicMeter = 1.225;
    double drivetrainEfficiency = 0.97;
    double rotatingMassFactor = 1.04;
    double lowSpeedMetersPerSecond = 1.5;
    double maximumDriveForceNewtons = 900.0;
    double maximumBrakeForceNewtons = 1200.0;
    double maximumSpeedMetersPerSecond = 30.0;
};

struct WorkoutGameRoadPhysicsInput
{
    double powerWatts = 0.0;
    double gradePercent = 0.0;
    double brake = 0.0;
};

struct WorkoutGameRoadPhysicsSnapshot
{
    bool ready = false;
    std::int64_t elapsedTimeMs = 0;
    std::int64_t droppedUpdateTimeMs = 0;
    double speedMetersPerSecond = 0.0;
    double distanceMeters = 0.0;
    double elevationMeters = 0.0;
};

class WorkoutGameRoadPhysics
{
public:
    bool configure(const WorkoutGameRoadPhysicsParameters &parameters);
    void reset(double initialSpeedMetersPerSecond = 0.0);
    WorkoutGameRoadPhysicsSnapshot update(
            const WorkoutGameRoadPhysicsInput &input,
            std::int64_t elapsedTimeMs);

    static bool validParameters(
            const WorkoutGameRoadPhysicsParameters &parameters);

private:
    void integrateStep(
            const WorkoutGameRoadPhysicsInput &input,
            double elapsedSeconds);
    WorkoutGameRoadPhysicsSnapshot snapshot() const;

    WorkoutGameRoadPhysicsParameters configuredParameters;
    bool configured = false;
    std::int64_t elapsedTimeMs = 0;
    std::int64_t droppedUpdateTimeMs = 0;
    double speedMetersPerSecond = 0.0;
    double distanceMeters = 0.0;
    double elevationMeters = 0.0;
};

#endif
