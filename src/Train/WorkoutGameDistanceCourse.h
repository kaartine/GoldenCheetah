/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameDistanceCourse_h
#define _GC_WorkoutGameDistanceCourse_h

#include "WorkoutGameCourse.h"
#include "WorkoutGameRoadPhysics.h"

#include <cstddef>
#include <cstdint>
#include <vector>

enum class WorkoutGameDistanceCourseStatus
{
    Ready,
    EmptyWorkout,
    InvalidWorkout,
    InvalidFtp,
    InvalidParameters,
    ResourceLimit,
    NoProgress
};

struct WorkoutGameDistanceCourseSection
{
    WorkoutGameFeature feature = WorkoutGameFeature::Trail;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    std::int64_t sourceStartMs = 0;
    std::int64_t nominalDurationMs = 0;
    std::int64_t minimumDurationMs = 0;
    std::int64_t maximumDurationMs = 0;
    double startDistanceMeters = 0.0;
    double lengthMeters = 0.0;
    double startElevationMeters = 0.0;
    double endElevationMeters = 0.0;
    double targetStartWatts = 0.0;
    double targetEndWatts = 0.0;
    double gradePercent = 0.0;
    double difficulty = 0.0;
    std::uint32_t visualVariant = 0;
    bool adjustableConnector = false;
};

struct WorkoutGameDistanceCourse
{
    WorkoutGameDistanceCourseStatus status =
            WorkoutGameDistanceCourseStatus::EmptyWorkout;
    std::uint32_t seed = 0;
    std::int64_t nominalDurationMs = 0;
    double totalDistanceMeters = 0.0;
    double elevationGainMeters = 0.0;
    double elevationLossMeters = 0.0;
    std::vector<WorkoutGameDistanceCourseSection> sections;
};

struct WorkoutGameDistanceCourseGenerationParameters
{
    WorkoutGameRoadPhysicsParameters roadPhysics;
    double recoveryIntensity = 0.65;
    double shortClimbIntensity = 1.1;
    std::int64_t shortClimbMaximumDurationMs = 120000;
    std::int64_t simulationStepMs = 50;
    std::int64_t maximumWorkoutDurationMs = 12 * 60 * 60 * 1000;
    std::size_t maximumSections = 10000;
};

struct WorkoutGameDistanceCourseEstimate
{
    bool finished = false;
    std::int64_t elapsedTimeMs = 0;
    double distanceMeters = 0.0;
    double elevationMeters = 0.0;
};

class WorkoutGameDistanceCourseBuilder
{
public:
    static WorkoutGameDistanceCourse build(
            const std::vector<WorkoutGameInterval> &intervals,
            double ftpWatts,
            const WorkoutGameDistanceCourseGenerationParameters &parameters =
                    WorkoutGameDistanceCourseGenerationParameters(),
            std::uint32_t requestedSeed = 0);

    static bool validParameters(
            const WorkoutGameDistanceCourseGenerationParameters &parameters);
};

class WorkoutGameDistanceCourseEstimator
{
public:
    static WorkoutGameDistanceCourseEstimate estimate(
            const WorkoutGameDistanceCourse &course,
            const WorkoutGameRoadPhysicsParameters &physicsParameters,
            double powerScale = 1.0,
            std::int64_t simulationStepMs = 50);
};

#endif
