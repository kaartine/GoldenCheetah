/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCourseConversion_h
#define _GC_WorkoutGameCourseConversion_h

#include "WorkoutGameDistanceCourse.h"

#include <cstdint>
#include <vector>

enum class WorkoutGameCoursePreset
{
    WorkoutFirst,
    Balanced,
    RideFirst
};

enum class WorkoutGameCourseConversionStatus
{
    Ready,
    InvalidInput,
    GenerationFailed,
    EstimateFailed
};

struct WorkoutGameCourseConversionRequest
{
    std::vector<WorkoutGameInterval> intervals;
    double ftpWatts = 0.0;
    WorkoutGameCoursePreset preset = WorkoutGameCoursePreset::Balanced;
    WorkoutGameRoadPhysicsParameters roadPhysics;
    std::uint32_t seed = 0;
};

struct WorkoutGameCourseConversionSummary
{
    std::int64_t nominalDurationMs = 0;
    double distanceMeters = 0.0;
    double elevationGainMeters = 0.0;
    double elevationLossMeters = 0.0;
    int climbCount = 0;
    int jumpCount = 0;
    int descentCount = 0;
    WorkoutGameDistanceCourseEstimate fastEstimate;
    WorkoutGameDistanceCourseEstimate nominalEstimate;
    WorkoutGameDistanceCourseEstimate slowEstimate;
};

struct WorkoutGameCourseConversionResult
{
    WorkoutGameCourseConversionStatus status =
            WorkoutGameCourseConversionStatus::InvalidInput;
    WorkoutGameCoursePreset preset = WorkoutGameCoursePreset::Balanced;
    WorkoutGameDistanceCourseGenerationParameters generationParameters;
    WorkoutGameDistanceCourse course;
    WorkoutGameCourseConversionSummary summary;
};

class WorkoutGameCourseConverter
{
public:
    static WorkoutGameCourseConversionResult convert(
            const WorkoutGameCourseConversionRequest &request);
    static WorkoutGameDistanceCourseGenerationParameters parametersForPreset(
            WorkoutGameCoursePreset preset,
            const WorkoutGameRoadPhysicsParameters &roadPhysics =
                    WorkoutGameRoadPhysicsParameters());
};

#endif
