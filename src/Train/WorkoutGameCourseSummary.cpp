/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCourseSummary.h"

#include "WorkoutGameCourseTerrain.h"
#include "WorkoutGameFeatureCatalog.h"

#include <cmath>

bool WorkoutGameCourseSummary::build(
        const WorkoutGameDistanceCourse &course,
        const std::vector<WorkoutGameInterval> &sourceIntervals,
        double ftpWatts,
        WorkoutGameCoursePreset preset,
        const WorkoutGameCoursePrescriptionAudit &prescription,
        const WorkoutGameRoadPhysicsParameters &roadPhysics,
        WorkoutGameCourseConversionSummary &summary)
{
    summary = WorkoutGameCourseConversionSummary();
    if (prescription.status != WorkoutGameCoursePrescriptionStatus::Ready
            || course.sections.size() != sourceIntervals.size()) {
        return false;
    }

    double eligibleDistance = 0.0;
    double technicalDistance = 0.0;
    int eligibleSections = 0;
    int technicalEligibleSections = 0;
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        const WorkoutGameDistanceCourseSection &section = course.sections[index];
        const bool technical =
                WorkoutGameFeatureCatalog::definition(section.terrain).technical;
        if (technical) ++summary.technicalFeatureCount;
        const bool eligible = !WorkoutGameCoursePrescription::isRecovery(
                    sourceIntervals[index], ftpWatts)
                && WorkoutGameCourseTerrain::paletteEligible(section.feature);
        if (eligible) {
            ++eligibleSections;
            eligibleDistance += section.lengthMeters;
            if (technical) {
                ++technicalEligibleSections;
                technicalDistance += section.lengthMeters;
            }
        }
        switch (section.feature) {
        case WorkoutGameFeature::Climb: ++summary.climbCount; break;
        case WorkoutGameFeature::SprintJump: ++summary.jumpCount; break;
        case WorkoutGameFeature::RecoveryDescent:
        case WorkoutGameFeature::CooldownDescent:
            ++summary.descentCount;
            break;
        default: break;
        }
    }

    summary.technicalTerrainExposureApplicable =
            eligibleSections > 0 && eligibleDistance > 0.0;
    if (eligibleSections > 0 && eligibleDistance > 0.0) {
        summary.technicalTerrainExposurePercent =
                100.0 * technicalDistance / eligibleDistance;
        summary.technicalFeatureDensityPerTenSections =
                10.0 * double(technicalEligibleSections)
                    / double(eligibleSections);
    }

    summary.sourceDurationMs = prescription.sourceDurationMs;
    summary.nominalDurationMs = course.nominalDurationMs;
    summary.distanceMeters = course.totalDistanceMeters;
    summary.elevationGainMeters = course.elevationGainMeters;
    summary.elevationLossMeters = course.elevationLossMeters;
    summary.sourceLoadPoints = prescription.sourceLoadPoints;
    summary.estimatedLoadPoints = prescription.generatedLoadPoints;
    summary.loadDeviationPercent = prescription.loadDeviationPercent;
    summary.workDurationDeviationPercent =
            prescription.workDurationDeviationPercent;
    summary.recoveryDurationDeviationPercent =
            prescription.recoveryDurationDeviationPercent;
    summary.totalDurationDeviationPercent =
            prescription.totalDurationDeviationPercent;
    summary.keyEffortCount = prescription.keyEffortCount;
    summary.preservedKeyEffortCount = prescription.preservedKeyEffortCount;
    summary.recoveryCount = prescription.recoveryCount;
    summary.preservedRecoveryCount = prescription.preservedRecoveryCount;
    summary.prescriptionChanges = prescription.durationChanges;
    summary.fastEstimate = WorkoutGameDistanceCourseEstimator::estimate(
            course, roadPhysics, 1.15, 250);
    summary.nominalEstimate = WorkoutGameDistanceCourseEstimator::estimate(
            course, roadPhysics, 1.0, 250);
    summary.slowEstimate = WorkoutGameDistanceCourseEstimator::estimate(
            course, roadPhysics, 0.85, 250);
    if (!summary.fastEstimate.finished
            || !summary.nominalEstimate.finished
            || !summary.slowEstimate.finished) {
        return false;
    }

    if (eligibleSections >= 10 && summary.technicalTerrainExposureApplicable) {
        const WorkoutGameCourseModeContract contract =
                WorkoutGameCoursePrescription::contractFor(preset);
        constexpr double Epsilon = 1.0e-9;
        if (summary.technicalFeatureDensityPerTenSections + Epsilon
                    < contract.minimumTechnicalFeatureDensityPerTenSections
                || summary.technicalFeatureDensityPerTenSections
                    > contract.maximumTechnicalFeatureDensityPerTenSections
                        + Epsilon) {
            return false;
        }
    }
    return true;
}
