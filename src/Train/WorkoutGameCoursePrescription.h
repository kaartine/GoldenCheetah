/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCoursePrescription_h
#define _GC_WorkoutGameCoursePrescription_h

#include "WorkoutGameCourse.h"

#include <cstdint>
#include <vector>

enum class WorkoutGameCoursePreset
{
    WorkoutFirst,
    Balanced,
    RideFirst
};

enum class WorkoutGameCourseIntervalRole
{
    Prescribed,
    NonPrescriptiveWarmup,
    NonPrescriptiveCooldown,
    NonPrescriptiveTransition
};

struct WorkoutGameCoursePrescriptionMetadata
{
    static constexpr int CurrentVersion = 1;

    // Version zero and no roles means that metadata was absent. In that case
    // every interval is prescribed. Any other unsupported shape is invalid.
    int version = 0;
    std::vector<WorkoutGameCourseIntervalRole> intervalRoles;

    bool present() const { return version != 0 || !intervalRoles.empty(); }
};

struct WorkoutGameCourseModeContract
{
    double maximumIntervalPowerErrorWatts = 0.0;
    std::int64_t maximumKeyEffortDurationErrorMs = 0;
    double minimumRecoveryRetention = 1.0;
    double defaultRecoveryRetention = 1.0;
    double minimumRecoveryExposure = 1.0;
    double maximumNonPrescriptiveDurationChangePercent = 0.0;
    double maximumWorkDurationDeviationPercent = 0.0;
    double maximumRecoveryDurationDeviationPercent = 0.0;
    double maximumTotalDurationDeviationPercent = 0.0;
    double maximumLoadDeviationPercent = 0.0;
    double targetMinimumTechnicalTerrainExposurePercent = 10.0;
    double targetMaximumTechnicalTerrainExposurePercent = 30.0;
    double minimumTechnicalFeatureDensityPerTenSections = 1.0;
    double maximumTechnicalFeatureDensityPerTenSections = 3.0;
};

enum class WorkoutGameCoursePrescriptionStatus
{
    Ready,
    InvalidInput,
    UnsupportedMetadataVersion,
    InvalidMetadata,
    ContractViolation
};

struct WorkoutGameCoursePrescriptionAudit
{
    WorkoutGameCoursePrescriptionStatus status =
            WorkoutGameCoursePrescriptionStatus::InvalidInput;
    std::int64_t sourceDurationMs = 0;
    std::int64_t generatedDurationMs = 0;
    std::int64_t sourceWorkDurationMs = 0;
    std::int64_t generatedWorkDurationMs = 0;
    std::int64_t sourceRecoveryDurationMs = 0;
    std::int64_t generatedRecoveryDurationMs = 0;
    double sourceLoadPoints = 0.0;
    double generatedLoadPoints = 0.0;
    double workDurationDeviationPercent = 0.0;
    double recoveryDurationDeviationPercent = 0.0;
    double totalDurationDeviationPercent = 0.0;
    double loadDeviationPercent = 0.0;
    int keyEffortCount = 0;
    int preservedKeyEffortCount = 0;
    int recoveryCount = 0;
    int preservedRecoveryCount = 0;
    struct DurationChange
    {
        std::size_t intervalIndex = 0;
        WorkoutGameCourseIntervalRole role =
                WorkoutGameCourseIntervalRole::Prescribed;
        std::int64_t sourceDurationMs = 0;
        std::int64_t generatedDurationMs = 0;
    };
    std::vector<DurationChange> durationChanges;
};

class WorkoutGameCoursePrescription
{
public:
    static WorkoutGameCourseModeContract contractFor(
            WorkoutGameCoursePreset preset);
    static bool isRecovery(
            const WorkoutGameInterval &interval,
            double ftpWatts);
    static bool isKeyEffort(
            const WorkoutGameInterval &interval,
            double ftpWatts);
    static WorkoutGameCoursePrescriptionStatus validateMetadata(
            const std::vector<WorkoutGameInterval> &intervals,
            double ftpWatts,
            const WorkoutGameCoursePrescriptionMetadata &metadata);
    static WorkoutGameCourseIntervalRole roleAt(
            const WorkoutGameCoursePrescriptionMetadata &metadata,
            std::size_t index);
    static WorkoutGameCoursePrescriptionAudit audit(
            const std::vector<WorkoutGameInterval> &source,
            const std::vector<WorkoutGameInterval> &generated,
            double ftpWatts,
            WorkoutGameCoursePreset preset,
            const WorkoutGameCoursePrescriptionMetadata &metadata = {});
};

#endif
