/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCoursePrescription.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace {

double averageIntensity(
        const WorkoutGameInterval &interval,
        double ftpWatts)
{
    return (interval.startWatts + interval.endWatts) * 0.5 / ftpWatts;
}

double deviationPercent(double generated, double source)
{
    return source > 0.0 ? 100.0 * (generated - source) / source : 0.0;
}

double loadPoints(
        const WorkoutGameInterval &interval,
        double ftpWatts)
{
    const double durationHours = double(interval.durationMs) / 3600000.0;
    const double powerSquared = interval.startWatts * interval.startWatts
            + interval.startWatts * interval.endWatts
            + interval.endWatts * interval.endWatts;
    return 100.0 * durationHours * powerSquared
            / (3.0 * ftpWatts * ftpWatts);
}

bool validPreset(WorkoutGameCoursePreset preset)
{
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst:
    case WorkoutGameCoursePreset::Balanced:
    case WorkoutGameCoursePreset::RideFirst:
        return true;
    }
    return false;
}

bool validIntervals(const std::vector<WorkoutGameInterval> &intervals)
{
    if (intervals.empty()) return false;
    std::int64_t expectedStart = 0;
    for (const WorkoutGameInterval &interval : intervals) {
        if (interval.startMs != expectedStart
                || interval.durationMs <= 0
                || !std::isfinite(interval.startWatts)
                || !std::isfinite(interval.endWatts)
                || interval.startWatts < 0.0
                || interval.endWatts < 0.0
                || interval.durationMs
                    > std::numeric_limits<std::int64_t>::max()
                        - expectedStart) {
            return false;
        }
        expectedStart += interval.durationMs;
    }
    return true;
}

}

WorkoutGameCourseModeContract WorkoutGameCoursePrescription::contractFor(
        WorkoutGameCoursePreset preset)
{
    WorkoutGameCourseModeContract contract;
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst:
        break;
    case WorkoutGameCoursePreset::Balanced:
        contract.maximumNonPrescriptiveDurationChangePercent = 3.0;
        contract.maximumWorkDurationDeviationPercent = 3.0;
        contract.maximumTotalDurationDeviationPercent = 3.0;
        contract.maximumLoadDeviationPercent = 3.0;
        contract.targetMinimumTechnicalTerrainExposurePercent = 50.0;
        contract.targetMaximumTechnicalTerrainExposurePercent = 75.0;
        contract.minimumTechnicalFeatureDensityPerTenSections = 5.0;
        contract.maximumTechnicalFeatureDensityPerTenSections = 7.0;
        break;
    case WorkoutGameCoursePreset::RideFirst:
        contract.maximumNonPrescriptiveDurationChangePercent = 8.0;
        contract.maximumWorkDurationDeviationPercent = 8.0;
        contract.maximumRecoveryDurationDeviationPercent = 8.0;
        contract.maximumTotalDurationDeviationPercent = 8.0;
        contract.maximumLoadDeviationPercent = 8.0;
        contract.targetMinimumTechnicalTerrainExposurePercent = 75.0;
        contract.targetMaximumTechnicalTerrainExposurePercent = 100.0;
        contract.minimumTechnicalFeatureDensityPerTenSections = 8.0;
        contract.maximumTechnicalFeatureDensityPerTenSections = 10.0;
        break;
    }
    return contract;
}

bool WorkoutGameCoursePrescription::isRecovery(
        const WorkoutGameInterval &interval,
        double ftpWatts)
{
    return std::isfinite(ftpWatts) && ftpWatts > 0.0
            && averageIntensity(interval, ftpWatts) <= 0.65;
}

bool WorkoutGameCoursePrescription::isKeyEffort(
        const WorkoutGameInterval &interval,
        double ftpWatts)
{
    if (!std::isfinite(ftpWatts) || ftpWatts <= 0.0
            || interval.durationMs <= 0
            || isRecovery(interval, ftpWatts)) {
        return false;
    }
    const double intensity = averageIntensity(interval, ftpWatts);
    return (interval.durationMs >= 120000 && intensity >= 0.85)
            || (interval.durationMs >= 20000 && intensity >= 1.05)
            || (interval.durationMs >= 10000 && intensity >= 1.20);
}

WorkoutGameCoursePrescriptionStatus
WorkoutGameCoursePrescription::validateMetadata(
        const std::vector<WorkoutGameInterval> &intervals,
        double ftpWatts,
        const WorkoutGameCoursePrescriptionMetadata &metadata)
{
    if (!validIntervals(intervals)
            || !std::isfinite(ftpWatts) || ftpWatts <= 0.0) {
        return WorkoutGameCoursePrescriptionStatus::InvalidInput;
    }
    if (!metadata.present()) {
        return WorkoutGameCoursePrescriptionStatus::Ready;
    }
    if (metadata.version != WorkoutGameCoursePrescriptionMetadata::CurrentVersion) {
        return WorkoutGameCoursePrescriptionStatus::UnsupportedMetadataVersion;
    }
    if (metadata.intervalRoles.size() != intervals.size()) {
        return WorkoutGameCoursePrescriptionStatus::InvalidMetadata;
    }
    for (std::size_t index = 0; index < intervals.size(); ++index) {
        switch (metadata.intervalRoles[index]) {
        case WorkoutGameCourseIntervalRole::Prescribed:
            break;
        case WorkoutGameCourseIntervalRole::NonPrescriptiveWarmup:
            if (index != 0) {
                return WorkoutGameCoursePrescriptionStatus::InvalidMetadata;
            }
            break;
        case WorkoutGameCourseIntervalRole::NonPrescriptiveCooldown:
            if (index + 1 != intervals.size()) {
                return WorkoutGameCoursePrescriptionStatus::InvalidMetadata;
            }
            break;
        case WorkoutGameCourseIntervalRole::NonPrescriptiveTransition:
            if (isRecovery(intervals[index], ftpWatts)
                    || isKeyEffort(intervals[index], ftpWatts)) {
                return WorkoutGameCoursePrescriptionStatus::InvalidMetadata;
            }
            break;
        default:
            return WorkoutGameCoursePrescriptionStatus::InvalidMetadata;
        }
    }
    return WorkoutGameCoursePrescriptionStatus::Ready;
}

WorkoutGameCourseIntervalRole WorkoutGameCoursePrescription::roleAt(
        const WorkoutGameCoursePrescriptionMetadata &metadata,
        std::size_t index)
{
    return metadata.present() && index < metadata.intervalRoles.size()
            ? metadata.intervalRoles[index]
            : WorkoutGameCourseIntervalRole::Prescribed;
}

WorkoutGameCoursePrescriptionAudit WorkoutGameCoursePrescription::audit(
        const std::vector<WorkoutGameInterval> &source,
        const std::vector<WorkoutGameInterval> &generated,
        double ftpWatts,
        WorkoutGameCoursePreset preset,
        const WorkoutGameCoursePrescriptionMetadata &metadata)
{
    WorkoutGameCoursePrescriptionAudit result;
    const WorkoutGameCoursePrescriptionStatus metadataStatus =
            validateMetadata(source, ftpWatts, metadata);
    if (metadataStatus != WorkoutGameCoursePrescriptionStatus::Ready) {
        result.status = metadataStatus;
        return result;
    }
    if (!validPreset(preset) || generated.size() != source.size()
            || !validIntervals(generated)) return result;

    const WorkoutGameCourseModeContract contract = contractFor(preset);
    bool accepted = true;
    std::int64_t expectedStart = 0;
    for (std::size_t index = 0; index < source.size(); ++index) {
        const WorkoutGameInterval &input = source[index];
        const WorkoutGameInterval &output = generated[index];
        if (output.startMs != expectedStart || output.durationMs <= 0
                || !std::isfinite(output.startWatts)
                || !std::isfinite(output.endWatts)) {
            accepted = false;
            break;
        }
        expectedStart += output.durationMs;
        const bool recovery = isRecovery(input, ftpWatts);
        const bool key = isKeyEffort(input, ftpWatts);
        const WorkoutGameCourseIntervalRole role = roleAt(metadata, index);
        const double durationChange = deviationPercent(
                double(output.durationMs), double(input.durationMs));

        result.sourceDurationMs += input.durationMs;
        result.generatedDurationMs += output.durationMs;
        result.sourceLoadPoints += loadPoints(input, ftpWatts);
        result.generatedLoadPoints += loadPoints(output, ftpWatts);
        if (recovery && role == WorkoutGameCourseIntervalRole::Prescribed) {
            ++result.recoveryCount;
            if (output.durationMs >= input.durationMs) {
                ++result.preservedRecoveryCount;
            }
            result.sourceRecoveryDurationMs += input.durationMs;
            result.generatedRecoveryDurationMs += output.durationMs;
        } else if (role == WorkoutGameCourseIntervalRole::Prescribed) {
            result.sourceWorkDurationMs += input.durationMs;
            result.generatedWorkDurationMs += output.durationMs;
        }
        if (key) {
            ++result.keyEffortCount;
            if (output.durationMs == input.durationMs) {
                ++result.preservedKeyEffortCount;
            }
        }
        if (output.durationMs != input.durationMs) {
            result.durationChanges.push_back({
                index, role, input.durationMs, output.durationMs
            });
        }

        accepted = accepted
                && std::abs(output.startWatts - input.startWatts)
                    <= contract.maximumIntervalPowerErrorWatts
                && std::abs(output.endWatts - input.endWatts)
                    <= contract.maximumIntervalPowerErrorWatts
                && (!key || std::llabs(output.durationMs - input.durationMs)
                    <= contract.maximumKeyEffortDurationErrorMs)
                && (!recovery
                    || role != WorkoutGameCourseIntervalRole::Prescribed
                    || double(output.durationMs) + 1.0
                        >= double(input.durationMs)
                            * contract.minimumRecoveryRetention);
        if (preset == WorkoutGameCoursePreset::WorkoutFirst
                || role == WorkoutGameCourseIntervalRole::Prescribed) {
            // A prescribed recovery can only be shortened in Ride first for a
            // separately validated safety reason. This transform supplies no
            // such authorization, so its default remains exact preservation.
            accepted = accepted && output.durationMs == input.durationMs;
        } else {
            accepted = accepted && std::abs(durationChange)
                    <= contract.maximumNonPrescriptiveDurationChangePercent
                        + 1.0e-9;
        }
    }

    result.workDurationDeviationPercent = deviationPercent(
            double(result.generatedWorkDurationMs),
            double(result.sourceWorkDurationMs));
    result.recoveryDurationDeviationPercent = deviationPercent(
            double(result.generatedRecoveryDurationMs),
            double(result.sourceRecoveryDurationMs));
    result.totalDurationDeviationPercent = deviationPercent(
            double(result.generatedDurationMs), double(result.sourceDurationMs));
    result.loadDeviationPercent = deviationPercent(
            result.generatedLoadPoints, result.sourceLoadPoints);
    accepted = accepted
            && std::abs(result.workDurationDeviationPercent)
                <= contract.maximumWorkDurationDeviationPercent + 1.0e-9
            && std::abs(result.recoveryDurationDeviationPercent)
                <= contract.maximumRecoveryDurationDeviationPercent + 1.0e-9
            && std::abs(result.totalDurationDeviationPercent)
                <= contract.maximumTotalDurationDeviationPercent + 1.0e-9
            && std::abs(result.loadDeviationPercent)
                <= contract.maximumLoadDeviationPercent + 1.0e-9;
    result.status = accepted
            ? WorkoutGameCoursePrescriptionStatus::Ready
            : WorkoutGameCoursePrescriptionStatus::ContractViolation;
    return result;
}
