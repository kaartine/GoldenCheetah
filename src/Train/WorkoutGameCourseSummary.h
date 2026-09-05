/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCourseSummary_h
#define _GC_WorkoutGameCourseSummary_h

#include "WorkoutGameCoursePrescription.h"
#include "WorkoutGameDistanceCourse.h"

#include <cstdint>
#include <vector>

struct WorkoutGameCourseConversionSummary
{
    std::int64_t sourceDurationMs = 0;
    std::int64_t nominalDurationMs = 0;
    double distanceMeters = 0.0;
    double elevationGainMeters = 0.0;
    double elevationLossMeters = 0.0;
    int climbCount = 0;
    int jumpCount = 0;
    int descentCount = 0;
    int technicalFeatureCount = 0;
    double sourceLoadPoints = 0.0;
    double estimatedLoadPoints = 0.0;
    double loadDeviationPercent = 0.0;
    double workDurationDeviationPercent = 0.0;
    double recoveryDurationDeviationPercent = 0.0;
    double totalDurationDeviationPercent = 0.0;
    int keyEffortCount = 0;
    int preservedKeyEffortCount = 0;
    int recoveryCount = 0;
    int preservedRecoveryCount = 0;
    bool technicalTerrainExposureApplicable = false;
    double technicalTerrainExposurePercent = 0.0;
    double technicalFeatureDensityPerTenSections = 0.0;
    std::vector<WorkoutGameCoursePrescriptionAudit::DurationChange>
            prescriptionChanges;
    WorkoutGameDistanceCourseEstimate fastEstimate;
    WorkoutGameDistanceCourseEstimate nominalEstimate;
    WorkoutGameDistanceCourseEstimate slowEstimate;
};

class WorkoutGameCourseSummary
{
public:
    static bool build(
            const WorkoutGameDistanceCourse &course,
            const std::vector<WorkoutGameInterval> &sourceIntervals,
            double ftpWatts,
            WorkoutGameCoursePreset preset,
            const WorkoutGameCoursePrescriptionAudit &prescription,
            const WorkoutGameRoadPhysicsParameters &roadPhysics,
            WorkoutGameCourseConversionSummary &summary);
};

#endif
