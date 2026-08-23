/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameWorld_h
#define _GC_WorkoutGameWorld_h

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

struct WorkoutGameRoadCourse;

enum class WorkoutGameTerrainKind
{
    SmoothTrail,
    Roots,
    Rollers,
    Climb,
    RockGarden,
    BunnyHop,
    Drop,
    Skinny,
    Berm,
    LogOver,
    Tabletop,
    RockSlab
};

struct WorkoutGameVehiclePose
{
    double distanceMeters = 0.0;
    double elevationMeters = 0.0;
    double pitchDegrees = 0.0;
    double rollDegrees = 0.0;
    double rearSuspension = 0.0;
    double frontSuspension = 0.0;
    double rearWheelRadians = 0.0;
    double frontWheelRadians = 0.0;
    double clearanceMeters = 0.0;
    bool airborne = false;
    bool walking = false;

    double airHeightMeters() const
    {
        if (!std::isfinite(clearanceMeters)) return 0.0;
        return std::clamp(clearanceMeters - 0.82, 0.0, 4.0);
    }
};

struct WorkoutGameWorldSnapshot
{
    bool ready = false;
    std::uint64_t generation = 0;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    std::uint32_t seed = 0;
    double gradePercent = 0.0;
    double difficulty = 0.0;
    double terrainOffsetMeters = 0.0;
    double surfaceElevationMeters = 0.0;
    WorkoutGameVehiclePose rider;
    double speedMetersPerSecond = 0.0;
    double landingImpact = 0.0;
};

struct WorkoutGamePhysicsInput
{
    std::int64_t workoutTimeMs = 0;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    double desiredSpeedMetersPerSecond = 0.0;
    double courseDistanceMeters = -1.0;
    double gradePercent = 0.0;
    double difficulty = 0.0;
    double effortRatio = 0.0;
    bool paused = false;
    bool jumpRequested = false;
    bool forceGroundFollowing = false;
    std::uint64_t featureActionId = 0;
};

class WorkoutGamePhysics
{
public:
    WorkoutGamePhysics();
    ~WorkoutGamePhysics();

    WorkoutGamePhysics(const WorkoutGamePhysics &) = delete;
    WorkoutGamePhysics &operator=(const WorkoutGamePhysics &) = delete;

    bool configure(std::uint32_t seed);
    bool configure(const WorkoutGameRoadCourse &course);
    void reset();
    WorkoutGameWorldSnapshot update(const WorkoutGamePhysicsInput &input);

    static double terrainHeight(
            WorkoutGameTerrainKind terrain,
            double distanceMeters,
            double gradePercent,
            double difficulty,
            std::uint32_t seed);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

enum class WorkoutGameCameraMode
{
    Side,
    ThreeQuarter,
    Chase
};

struct WorkoutGameCameraSnapshot
{
    bool ready = false;
    WorkoutGameCameraMode mode = WorkoutGameCameraMode::Side;
    double centerDistanceMeters = 0.0;
    double centerElevationMeters = 0.0;
    double lookAheadMeters = 0.0;
    double zoom = 1.0;
    double yawDegrees = 90.0;
    double pitchDegrees = 0.0;
};

class WorkoutGameCamera
{
public:
    void reset();
    WorkoutGameCameraSnapshot update(
            const WorkoutGameWorldSnapshot &world,
            double elapsedSeconds);

    static WorkoutGameCameraMode preferredMode(
            WorkoutGameTerrainKind terrain);

private:
    bool initialized = false;
    std::uint64_t currentGeneration = 0;
    WorkoutGameCameraSnapshot current;
};

#endif
