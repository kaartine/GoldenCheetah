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

#include <algorithm>
#include <cmath>
#include <set>

namespace {

std::size_t sourceIntervalAt(
        const std::vector<WorkoutGameInterval> &sourceIntervals,
        std::int64_t sourceStartMs)
{
    const auto upper = std::upper_bound(
            sourceIntervals.begin(), sourceIntervals.end(), sourceStartMs,
            [](std::int64_t value, const WorkoutGameInterval &interval) {
                return value < interval.startMs;
            });
    if (upper == sourceIntervals.begin()) return sourceIntervals.size();
    const auto candidate = std::prev(upper);
    if (sourceStartMs >= candidate->startMs + candidate->durationMs) {
        return sourceIntervals.size();
    }
    return std::size_t(std::distance(sourceIntervals.begin(), candidate));
}

}

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
            || course.sections.empty()
            || sourceIntervals.empty()) {
        return false;
    }

    double eligibleDistance = 0.0;
    double technicalDistance = 0.0;
    double routeDistance = 0.0;
    double technicalRouteDistance = 0.0;
    int eligibleSections = 0;
    int technicalEligibleSections = 0;
    int technicalRouteSections = 0;
    std::set<WorkoutGameTerrainKind> featureKinds;
    std::set<std::size_t> countedClimbs;
    std::set<std::size_t> countedJumps;
    std::set<std::size_t> countedDescents;
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        const WorkoutGameDistanceCourseSection &section = course.sections[index];
        const std::size_t sourceIndex = sourceIntervalAt(
                sourceIntervals, section.sourceStartMs);
        if (sourceIndex >= sourceIntervals.size()) return false;
        const bool technical =
                WorkoutGameFeatureCatalog::definition(section.terrain).technical;
        routeDistance += section.lengthMeters;
        if (technical || section.terrain == WorkoutGameTerrainKind::Drop) {
            ++technicalRouteSections;
            technicalRouteDistance += section.lengthMeters;
        }
        if (section.challengeCount != 0) switch (section.terrain) {
        case WorkoutGameTerrainKind::Roots:
        case WorkoutGameTerrainKind::Rollers:
        case WorkoutGameTerrainKind::RockGarden:
        case WorkoutGameTerrainKind::BunnyHop:
        case WorkoutGameTerrainKind::Drop:
        case WorkoutGameTerrainKind::Skinny:
        case WorkoutGameTerrainKind::LogOver:
        case WorkoutGameTerrainKind::Tabletop:
        case WorkoutGameTerrainKind::RockSlab:
        case WorkoutGameTerrainKind::GapJump:
            featureKinds.insert(section.terrain);
            break;
        default:
            break;
        }
        if (technical) ++summary.technicalFeatureCount;
        const bool eligible = !WorkoutGameCoursePrescription::isRecovery(
                    sourceIntervals[sourceIndex], ftpWatts)
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
        case WorkoutGameFeature::Climb:
            countedClimbs.insert(sourceIndex);
            break;
        case WorkoutGameFeature::SprintJump:
            countedJumps.insert(sourceIndex);
            break;
        case WorkoutGameFeature::RecoveryDescent:
        case WorkoutGameFeature::CooldownDescent:
            countedDescents.insert(sourceIndex);
            break;
        default: break;
        }
    }
    summary.climbCount = int(countedClimbs.size());
    summary.jumpCount = int(countedJumps.size());
    summary.descentCount = int(countedDescents.size());

    summary.technicalTerrainExposureApplicable =
            eligibleSections >= 2 && eligibleDistance > 0.0;
    if (summary.technicalTerrainExposureApplicable) {
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

    constexpr std::size_t CompleteFeaturePaletteSize = 10u;
    const bool completeFeatureShowcase =
            featureKinds.size() == CompleteFeaturePaletteSize;
    summary.completeFeatureShowcase = completeFeatureShowcase;
    if (completeFeatureShowcase && routeDistance > 0.0) {
        summary.technicalTerrainExposureApplicable = true;
        summary.technicalTerrainExposurePercent =
                100.0 * technicalRouteDistance / routeDistance;
        summary.technicalFeatureDensityPerTenSections =
                10.0 * double(technicalRouteSections)
                    / double(course.sections.size());
    }
    if (eligibleSections >= 10 && summary.technicalTerrainExposureApplicable
            && !completeFeatureShowcase) {
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
