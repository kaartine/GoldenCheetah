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

#include "WorkoutGameCoursePrescription.h"
#include "WorkoutGameCourseSummary.h"
#include "WorkoutGameDistanceCourse.h"

#include <cstdint>
#include <vector>

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
    WorkoutGameCoursePrescriptionMetadata prescriptionMetadata;
    std::uint32_t seed = 0;
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
