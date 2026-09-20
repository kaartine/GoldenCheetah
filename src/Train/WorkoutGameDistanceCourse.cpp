/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameDistanceCourse.h"
#include "WorkoutGameClimbGeometry.h"
#include "WorkoutGameCourseTerrain.h"
#include "WorkoutGameGapJumpGeometry.h"
#include "WorkoutGameTabletopGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double MinimumSectionLengthMeters = 0.01;
constexpr std::int64_t MaximumEstimateDurationMs = 48 * 60 * 60 * 1000;
constexpr std::int64_t MaximumVisualSectionDurationMs = 60 * 1000;
constexpr double FeatureRichCourseMinimumDistanceMeters = 5000.0;

double averageWatts(const WorkoutGameInterval &interval)
{
    return (interval.startWatts + interval.endWatts) * 0.5;
}

double prescribedTargetAt(
        const WorkoutGameDistanceCourseSection &section,
        double progress)
{
    const double clamped = std::clamp(progress, 0.0, 1.0);
    return section.targetStartWatts
            + (section.targetEndWatts - section.targetStartWatts) * clamped;
}

double referenceEffortAt(
        const WorkoutGameDistanceCourseSection &section,
        double progress)
{
    const bool hasReferenceEffort = section.referenceEffortStartWatts >= 0.0
            && section.referenceEffortEndWatts >= 0.0;
    const double start = hasReferenceEffort
            ? section.referenceEffortStartWatts : section.targetStartWatts;
    const double end = hasReferenceEffort
            ? section.referenceEffortEndWatts : section.targetEndWatts;
    const double clamped = std::clamp(progress, 0.0, 1.0);
    return start + (end - start) * clamped;
}

bool isRecoveryFeature(WorkoutGameFeature feature)
{
    return feature == WorkoutGameFeature::RecoveryDescent
            || feature == WorkoutGameFeature::CooldownDescent;
}

double recoveryGrade(
        WorkoutGameFeature feature,
        std::int64_t durationMs)
{
    const double durationSeconds = std::max(
            1.0, double(durationMs) / 1000.0);
    if (feature == WorkoutGameFeature::CooldownDescent) {
        return -(0.70 + std::min(0.80, 60.0 / durationSeconds));
    }
    return -(0.90 + std::clamp(
            90.0 / durationSeconds, 0.15, 1.50));
}

WorkoutGameSection adaptSection(
        const WorkoutGameSection &source,
        const WorkoutGameInterval &interval,
        double ftpWatts,
        const WorkoutGameDistanceCourseGenerationParameters &parameters,
        bool first,
        bool last)
{
    WorkoutGameSection result = source;
    const double intensity = averageWatts(interval) / ftpWatts;

    if (!first && !last && intensity <= parameters.recoveryIntensity) {
        result.feature = WorkoutGameFeature::RecoveryDescent;
        result.terrain = WorkoutGameTerrainKind::Drop;
        result.gradePercent = recoveryGrade(
                result.feature, interval.durationMs);
        result.gravityAssisted = true;
        result.challengeCount = 0;
    } else if (!first && !last
            && result.feature != WorkoutGameFeature::SprintJump
            && interval.durationMs > 20000
            && interval.durationMs <= parameters.shortClimbMaximumDurationMs
            && intensity >= parameters.shortClimbIntensity) {
        result.feature = WorkoutGameFeature::Climb;
        result.terrain = WorkoutGameTerrainKind::Climb;
        result.gradePercent = 4.0 + 3.0 * result.difficulty;
        result.gravityAssisted = false;
        result.challengeCount = 1;
    }

    if (result.feature == WorkoutGameFeature::CooldownDescent) {
        result.gradePercent = recoveryGrade(
                result.feature, interval.durationMs);
    }
    result.gradePercent *= parameters.gradeScale;
    return result;
}

std::int64_t scaledDuration(
        std::int64_t durationMs,
        double scale)
{
    const double scaled = double(durationMs) * scale;
    return std::max<std::int64_t>(1, std::llround(scaled));
}

std::int64_t proportionalBoundary(
        std::int64_t durationMs,
        int part,
        int partCount)
{
    return durationMs * part / partCount;
}

bool subdivideLongSections(
        std::vector<WorkoutGameDistanceCourseSection> &sections,
        std::vector<WorkoutGameSection> &adaptedSections,
        std::vector<bool> &sourceRecoveries,
        const std::vector<bool> &featureGeometryReservations,
        double maximumLengthMeters,
        std::size_t maximumSections)
{
    if (featureGeometryReservations.size() != sections.size()) return false;
    std::size_t required = 0;
    for (std::size_t index = 0; index < sections.size(); ++index) {
        const WorkoutGameDistanceCourseSection &section = sections[index];
        const WorkoutGameFeature feature = adaptedSections[index].feature;
        const double samplingLengthMeters =
                featureGeometryReservations[index]
                    || feature == WorkoutGameFeature::SprintJump
                    || feature == WorkoutGameFeature::Climb
            ? maximumLengthMeters : maximumLengthMeters / 3.0;
        const std::int64_t timeParts = std::max<std::int64_t>(
                1, (section.nominalDurationMs
                    + MaximumVisualSectionDurationMs - 1)
                        / MaximumVisualSectionDurationMs);
        const std::int64_t distanceParts = std::max<std::int64_t>(
                1, std::int64_t(std::ceil(
                    section.lengthMeters / samplingLengthMeters)));
        required += std::size_t(std::max(timeParts, distanceParts));
        if (required > maximumSections) return false;
    }

    std::vector<WorkoutGameDistanceCourseSection> subdivided;
    std::vector<WorkoutGameSection> subdividedAdapted;
    std::vector<bool> subdividedRecoveries;
    subdivided.reserve(required);
    subdividedAdapted.reserve(required);
    subdividedRecoveries.reserve(required);
    for (std::size_t index = 0; index < sections.size(); ++index) {
        const WorkoutGameDistanceCourseSection &source = sections[index];
        const WorkoutGameSection &adapted = adaptedSections[index];
        const double samplingLengthMeters =
                featureGeometryReservations[index]
                    || adapted.feature == WorkoutGameFeature::SprintJump
                    || adapted.feature == WorkoutGameFeature::Climb
            ? maximumLengthMeters : maximumLengthMeters / 3.0;
        const std::int64_t timeParts = std::max<std::int64_t>(
                1, (source.nominalDurationMs
                    + MaximumVisualSectionDurationMs - 1)
                        / MaximumVisualSectionDurationMs);
        const std::int64_t distanceParts = std::max<std::int64_t>(
                1, std::int64_t(std::ceil(
                    source.lengthMeters / samplingLengthMeters)));
        const int partCount = int(std::max(timeParts, distanceParts));
        for (int part = 0; part < partCount; ++part) {
            const std::int64_t startOffset = proportionalBoundary(
                    source.nominalDurationMs, part, partCount);
            const std::int64_t endOffset = proportionalBoundary(
                    source.nominalDurationMs, part + 1, partCount);
            const double startProgress = double(startOffset)
                    / double(source.nominalDurationMs);
            const double endProgress = double(endOffset)
                    / double(source.nominalDurationMs);

            WorkoutGameDistanceCourseSection section = source;
            section.sourceStartMs = source.sourceStartMs + startOffset;
            section.nominalDurationMs = endOffset - startOffset;
            const std::int64_t minimumStart = proportionalBoundary(
                    source.minimumDurationMs, part, partCount);
            const std::int64_t minimumEnd = proportionalBoundary(
                    source.minimumDurationMs, part + 1, partCount);
            const std::int64_t maximumStart = proportionalBoundary(
                    source.maximumDurationMs, part, partCount);
            const std::int64_t maximumEnd = proportionalBoundary(
                    source.maximumDurationMs, part + 1, partCount);
            section.minimumDurationMs = std::max<std::int64_t>(
                    1, minimumEnd - minimumStart);
            section.maximumDurationMs = std::max(
                    section.nominalDurationMs, maximumEnd - maximumStart);
            section.startDistanceMeters = source.startDistanceMeters
                    + source.lengthMeters * startProgress;
            section.lengthMeters = source.lengthMeters
                    * (endProgress - startProgress);
            section.startElevationMeters = source.startElevationMeters
                    + (source.endElevationMeters
                       - source.startElevationMeters) * startProgress;
            section.endElevationMeters = source.startElevationMeters
                    + (source.endElevationMeters
                       - source.startElevationMeters) * endProgress;
            section.targetStartWatts = prescribedTargetAt(source, startProgress);
            section.targetEndWatts = prescribedTargetAt(source, endProgress);
            section.visualVariant = source.visualVariant
                    ^ std::uint32_t((part + 1) * 0x9e3779b9u);

            WorkoutGameSection visual = adapted;
            visual.startMs = section.sourceStartMs;
            visual.durationMs = section.nominalDurationMs;
            visual.targetWatts = (section.targetStartWatts
                    + section.targetEndWatts) * 0.5;
            visual.lengthMeters = section.lengthMeters;
            visual.visualVariant = section.visualVariant;
            subdivided.push_back(section);
            subdividedAdapted.push_back(visual);
            subdividedRecoveries.push_back(sourceRecoveries[index]);
        }
    }
    sections = std::move(subdivided);
    adaptedSections = std::move(subdividedAdapted);
    sourceRecoveries = std::move(subdividedRecoveries);
    return true;
}

double asymmetricTerrainSignal(
        double distanceMeters,
        double wavelengthMeters,
        std::uint32_t seed)
{
    constexpr double Pi = 3.14159265358979323846;
    constexpr double RiseShare = 0.65;
    const double seedPhase = double(seed % 1009u) / 1009.0;
    double phase = std::fmod(distanceMeters / wavelengthMeters + seedPhase, 1.0);
    if (phase < 0.0) phase += 1.0;
    if (phase < RiseShare) {
        return -std::cos(Pi * phase / RiseShare);
    }
    return std::cos(Pi * (phase - RiseShare) / (1.0 - RiseShare));
}

void applyTerrainEffortProfile(
        WorkoutGameDistanceCourse &course,
        double ftpWatts,
        const WorkoutGameDistanceCourseGenerationParameters &parameters)
{
    if (course.sections.empty()) return;

    const double amplitude = parameters.terrainVariationPercent / 100.0;
    std::vector<double> terrainSignals(course.sections.size() + 1u, 0.0);
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        terrainSignals[index] = asymmetricTerrainSignal(
                course.sections[index].startDistanceMeters,
                parameters.variationLengthMeters, course.seed);
    }
    const WorkoutGameDistanceCourseSection &last = course.sections.back();
    terrainSignals.back() = asymmetricTerrainSignal(
            last.startDistanceMeters + last.lengthMeters,
            parameters.variationLengthMeters, course.seed);

    // Weight shared boundary values exactly as the piecewise-linear effort
    // curve will be integrated. Subtracting this mean preserves the source
    // workout's total effort even when its prescribed watts change.
    double weightedSignal = 0.0;
    double totalWeight = 0.0;
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        const WorkoutGameDistanceCourseSection &section = course.sections[index];
        const double halfDuration = 0.5 * double(section.nominalDurationMs);
        const double startWeight = section.targetStartWatts * halfDuration;
        const double endWeight = section.targetEndWatts * halfDuration;
        weightedSignal += terrainSignals[index] * startWeight
                + terrainSignals[index + 1u] * endWeight;
        totalWeight += startWeight + endWeight;
    }
    const double meanSignal = totalWeight > 0.0
            ? weightedSignal / totalWeight : 0.0;
    double maximumMagnitude = 0.0;
    for (double &signal : terrainSignals) {
        signal -= meanSignal;
        maximumMagnitude = std::max(maximumMagnitude, std::abs(signal));
    }
    if (maximumMagnitude > 0.0) {
        for (double &signal : terrainSignals) signal /= maximumMagnitude;
    }

    course.elevationGainMeters = 0.0;
    course.elevationLossMeters = 0.0;
    double elevationMeters = 0.0;
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        WorkoutGameDistanceCourseSection &section = course.sections[index];
        const double startFactor = 1.0 + amplitude * terrainSignals[index];
        const double endFactor = 1.0 + amplitude * terrainSignals[index + 1u];
        section.referenceEffortStartWatts = std::max(
                0.0, section.targetStartWatts * startFactor);
        section.referenceEffortEndWatts = std::max(
                0.0, section.targetEndWatts * endFactor);
        const double referenceEffort = 0.5
                * (section.referenceEffortStartWatts
                   + section.referenceEffortEndWatts);
        const double intensity = referenceEffort / ftpWatts;
        section.gradePercent = std::clamp(
                (intensity - 0.45) * 10.0 * parameters.gradeScale,
                -3.0, 12.0);
        section.startElevationMeters = elevationMeters;
        const double riseMeters = section.lengthMeters
                * section.gradePercent / 100.0;
        elevationMeters += riseMeters;
        section.endElevationMeters = elevationMeters;
        course.elevationGainMeters += std::max(0.0, riseMeters);
        course.elevationLossMeters += std::max(0.0, -riseMeters);
    }
}

double averagePrescribedEffort(
        const WorkoutGameDistanceCourseSection &section)
{
    return 0.5 * (section.targetStartWatts + section.targetEndWatts);
}

struct EffortSemantics
{
    bool falling = false;
    bool rising = false;
    bool sustainedHigh = false;

    bool hasFeature() const
    {
        return falling || rising || sustainedHigh;
    }
};

EffortSemantics effortSemanticsAt(
        const WorkoutGameDistanceCourse &course,
        std::size_t index,
        double ftpWatts)
{
    EffortSemantics result;
    if (index >= course.sections.size()) return result;
    const WorkoutGameDistanceCourseSection &section = course.sections[index];
    const double effort = averagePrescribedEffort(section);
    const double previousEffort = index == 0
            ? effort : averagePrescribedEffort(course.sections[index - 1]);
    const double nextEffort = index + 1 == course.sections.size()
            ? effort : averagePrescribedEffort(course.sections[index + 1]);
    const double threshold = 0.03 * ftpWatts;
    const double localChange = section.targetEndWatts
            - section.targetStartWatts;
    result.falling = localChange <= -threshold
            || nextEffort - effort <= -threshold;
    result.rising = localChange >= threshold
            || effort - previousEffort >= threshold
            || nextEffort - effort >= threshold;
    result.sustainedHigh = effort >= 0.90 * ftpWatts
            && std::abs(localChange) < threshold
            && std::abs(nextEffort - effort) < threshold;
    return result;
}

void alignTechnicalTerrainWithEffort(
        WorkoutGameSection &visual,
        const WorkoutGameDistanceCourse &course,
        std::size_t index,
        double ftpWatts,
        WorkoutGameCoursePreset preset,
        const WorkoutGameCourseTerrainSelection &selection,
        bool sourceRecovery)
{
    if (sourceRecovery || index >= course.sections.size()) {
        return;
    }
    const bool alreadyTechnical = visual.feature == WorkoutGameFeature::Climb
            || visual.feature == WorkoutGameFeature::SprintJump;
    const WorkoutGameDistanceCourseSection &section = course.sections[index];
    const EffortSemantics semantics =
            effortSemanticsAt(course, index, ftpWatts);

    // Effort semantics decide where a feature belongs. Palette density still
    // controls ordinary decoration, but it must not hide a real acceleration,
    // release or sustained hard effort from the generated terrain.
    if (!selection.technical && !alreadyTechnical && !semantics.falling) {
        return;
    }

    if (semantics.falling) {
        visual.feature = WorkoutGameFeature::SprintJump;
        visual.terrain = WorkoutGameTerrainKind::Drop;
    } else if (semantics.rising) {
        visual.feature = WorkoutGameFeature::SprintJump;
        if (preset == WorkoutGameCoursePreset::WorkoutFirst) {
            visual.terrain = WorkoutGameTerrainKind::Rollers;
        } else if (preset == WorkoutGameCoursePreset::Balanced) {
            visual.terrain = selection.ordinal % 2u == 0u
                    ? WorkoutGameTerrainKind::RockSlab
                    : WorkoutGameTerrainKind::Tabletop;
        } else if (section.lengthMeters >= 38.0
                && selection.ordinal % 2u == 0u) {
            visual.terrain = WorkoutGameTerrainKind::GapJump;
        } else {
            visual.terrain = WorkoutGameTerrainKind::RockSlab;
        }
    } else if (semantics.sustainedHigh) {
        switch (selection.ordinal % 3u) {
        case 0u:
            visual.feature = WorkoutGameFeature::Climb;
            visual.terrain = WorkoutGameTerrainKind::Climb;
            break;
        case 1u:
            visual.terrain = WorkoutGameTerrainKind::RockSlab;
            break;
        default:
            visual.terrain = WorkoutGameTerrainKind::RockGarden;
            break;
        }
    }
    if (visual.terrain == WorkoutGameTerrainKind::Climb
            && section.lengthMeters + 1.0e-9
                < WorkoutGameClimbGeometry::profile(
                    visual.difficulty).minimumLengthMeters) {
        visual.feature = WorkoutGameFeature::Trail;
        visual.terrain = WorkoutGameTerrainKind::RockSlab;
    }
    if (visual.terrain == WorkoutGameTerrainKind::Tabletop) {
        const WorkoutGameTabletopGeometryProfile tabletop =
                WorkoutGameTabletopGeometry::profile(visual.difficulty);
        const double minimumLength = std::max(
                tabletop.splitLeadMeters
                    + tabletop.minimumBypassLengthMeters,
                2.0 * tabletop.splitLeadMeters + 4.0
                    + tabletop.endMeters
                    + tabletop.bypassExitRunoutMeters);
        if (!tabletop.ready
                || section.lengthMeters + 1.0e-9 < minimumLength) {
            visual.terrain = WorkoutGameTerrainKind::RockSlab;
        }
    }
    if (visual.terrain == WorkoutGameTerrainKind::GapJump) {
        const WorkoutGameGapJumpGeometryProfile authority =
                WorkoutGameGapJumpGeometry::profile(visual.difficulty);
        const WorkoutGameGapJumpGeometryProfile geometry =
                WorkoutGameGapJumpGeometry::canonicalProfile();
        const double maximumGap = geometry.ready
                ? geometry.lines.back().gapLengthMeters : 0.0;
        const double minimumLength = authority.prepareLeadMeters
                + maximumGap + 6.0 + geometry.mergeLengthMeters;
        if (!authority.ready || !geometry.ready
                || section.lengthMeters + 1.0e-9 < minimumLength) {
            visual.terrain = WorkoutGameTerrainKind::RockSlab;
        }
    }
    visual.challengeCount = visual.terrain
            == WorkoutGameTerrainKind::SmoothTrail
        ? 0 : std::max(1, visual.challengeCount);
}

void distributeShowcaseSelections(
        std::vector<WorkoutGameCourseTerrainSelection> &selections)
{
    const std::size_t selectedCount = std::size_t(std::count_if(
            selections.begin(), selections.end(),
            [](const WorkoutGameCourseTerrainSelection &selection) {
                return selection.technical;
            }));
    if (selectedCount == 0u) return;

    std::vector<WorkoutGameCourseTerrainSelection> distributed(
            selections.size());
    for (std::size_t ordinal = 0; ordinal < selectedCount; ++ordinal) {
        const std::size_t index = selectedCount == 1u
                ? 0u
                : std::size_t(std::llround(
                    double(ordinal) * double(selections.size() - 1u)
                        / double(selectedCount - 1u)));
        distributed[index] = {true, ordinal, true};
    }
    selections = std::move(distributed);
}

WorkoutGameDistanceCourseStatus mapStatus(WorkoutGameCourseStatus status)
{
    switch (status) {
    case WorkoutGameCourseStatus::Ready:
        return WorkoutGameDistanceCourseStatus::Ready;
    case WorkoutGameCourseStatus::EmptyWorkout:
        return WorkoutGameDistanceCourseStatus::EmptyWorkout;
    case WorkoutGameCourseStatus::InvalidFtp:
        return WorkoutGameDistanceCourseStatus::InvalidFtp;
    case WorkoutGameCourseStatus::InvalidInterval:
        return WorkoutGameDistanceCourseStatus::InvalidWorkout;
    }
    return WorkoutGameDistanceCourseStatus::InvalidWorkout;
}

bool validCourseForEstimate(const WorkoutGameDistanceCourse &course)
{
    if (course.status != WorkoutGameDistanceCourseStatus::Ready
            || course.sections.empty()
            || course.sections.size() > 10000
            || course.nominalDurationMs <= 0
            || course.nominalDurationMs > MaximumEstimateDurationMs
            || !std::isfinite(course.totalDistanceMeters)
            || course.totalDistanceMeters <= 0.0
            || !std::isfinite(course.elevationGainMeters)
            || !std::isfinite(course.elevationLossMeters)
            || course.elevationGainMeters < 0.0
            || course.elevationLossMeters < 0.0) {
        return false;
    }

    double expectedStart = 0.0;
    std::int64_t expectedTime = 0;
    for (const WorkoutGameDistanceCourseSection &section : course.sections) {
        if (!std::isfinite(section.startDistanceMeters)
                || !std::isfinite(section.lengthMeters)
                || !std::isfinite(section.startElevationMeters)
                || !std::isfinite(section.endElevationMeters)
                || !std::isfinite(section.targetStartWatts)
                || !std::isfinite(section.targetEndWatts)
                || !std::isfinite(section.referenceEffortStartWatts)
                || !std::isfinite(section.referenceEffortEndWatts)
                || !std::isfinite(section.gradePercent)
                || !std::isfinite(section.difficulty)
                || std::abs(section.startDistanceMeters - expectedStart) > 0.001
                || section.sourceStartMs != expectedTime
                || section.nominalDurationMs <= 0
                || section.minimumDurationMs <= 0
                || section.maximumDurationMs < section.nominalDurationMs
                || section.minimumDurationMs > section.nominalDurationMs
                || section.lengthMeters < MinimumSectionLengthMeters
                || section.targetStartWatts < 0.0
                || section.targetEndWatts < 0.0
                || (section.referenceEffortStartWatts < 0.0
                    && section.referenceEffortStartWatts != -1.0)
                || (section.referenceEffortEndWatts < 0.0
                    && section.referenceEffortEndWatts != -1.0)
                || ((section.referenceEffortStartWatts < 0.0)
                    != (section.referenceEffortEndWatts < 0.0))
                || section.gradePercent < -30.0
                || section.gradePercent > 30.0
                || section.difficulty < 0.0
                || section.difficulty > 1.0
                || section.challengeCount < -1
                || section.challengeCount > 1000000
                || section.nominalDurationMs
                    > course.nominalDurationMs - expectedTime) {
            return false;
        }
        expectedStart += section.lengthMeters;
        expectedTime += section.nominalDurationMs;
        if (!std::isfinite(expectedStart)) return false;
    }
    return std::abs(expectedStart - course.totalDistanceMeters) <= 0.001
            && expectedTime == course.nominalDurationMs;
}

}

bool WorkoutGameDistanceCourseBuilder::validParameters(
        const WorkoutGameDistanceCourseGenerationParameters &parameters)
{
    return WorkoutGameRoadPhysics::validParameters(parameters.roadPhysics)
            && std::isfinite(parameters.recoveryIntensity)
            && parameters.recoveryIntensity >= 0.0
            && parameters.recoveryIntensity <= 1.0
            && std::isfinite(parameters.shortClimbIntensity)
            && parameters.shortClimbIntensity > parameters.recoveryIntensity
            && std::isfinite(parameters.gradeScale)
            && parameters.gradeScale >= 0.5
            && parameters.gradeScale <= 1.5
            && std::isfinite(parameters.technicality)
            && parameters.technicality >= 0.0
            && parameters.technicality <= 1.0
            && std::isfinite(parameters.terrainVariationPercent)
            && parameters.terrainVariationPercent >= 0.0
            && parameters.terrainVariationPercent <= 30.0
            && std::isfinite(parameters.variationLengthMeters)
            && parameters.variationLengthMeters >= 20.0
            && parameters.variationLengthMeters <= 200.0
            && parameters.referenceGear >= 1
            && parameters.referenceGear <= 12
            && std::isfinite(parameters.workMinimumDurationScale)
            && parameters.workMinimumDurationScale > 0.0
            && parameters.workMinimumDurationScale <= 1.0
            && std::isfinite(parameters.workMaximumDurationScale)
            && parameters.workMaximumDurationScale >= 1.0
            && parameters.workMaximumDurationScale <= 3.0
            && std::isfinite(parameters.recoveryMinimumDurationScale)
            && parameters.recoveryMinimumDurationScale > 0.0
            && parameters.recoveryMinimumDurationScale <= 1.0
            && std::isfinite(parameters.recoveryMaximumDurationScale)
            && parameters.recoveryMaximumDurationScale >= 1.0
            && parameters.recoveryMaximumDurationScale <= 3.0
            && parameters.shortClimbMaximumDurationMs > 20000
            && parameters.simulationStepMs >= 10
            && parameters.simulationStepMs <= 1000
            && parameters.maximumWorkoutDurationMs > 0
            && parameters.maximumWorkoutDurationMs
                    <= MaximumEstimateDurationMs
            && parameters.maximumSections > 0
            && parameters.maximumSections <= 1000000;
}

bool WorkoutGameDistanceCourseBuilder::validCourse(
        const WorkoutGameDistanceCourse &course)
{
    return validCourseForEstimate(course);
}

WorkoutGameDistanceCourse WorkoutGameDistanceCourseBuilder::build(
        const std::vector<WorkoutGameInterval> &intervals,
        double ftpWatts,
        const WorkoutGameDistanceCourseGenerationParameters &parameters,
        std::uint32_t requestedSeed)
{
    WorkoutGameDistanceCourse result;
    if (!validParameters(parameters)) {
        result.status = WorkoutGameDistanceCourseStatus::InvalidParameters;
        return result;
    }
    if (intervals.size() > parameters.maximumSections) {
        result.status = WorkoutGameDistanceCourseStatus::ResourceLimit;
        return result;
    }

    const WorkoutGameCourse source = WorkoutGameCourseBuilder::build(
            intervals, ftpWatts, requestedSeed);
    result.status = mapStatus(source.status);
    if (result.status != WorkoutGameDistanceCourseStatus::Ready) return result;
    if (source.durationMs > parameters.maximumWorkoutDurationMs) {
        result.status = WorkoutGameDistanceCourseStatus::ResourceLimit;
        return result;
    }

    WorkoutGameRoadPhysics physics;
    if (!physics.configure(parameters.roadPhysics)) {
        result.status = WorkoutGameDistanceCourseStatus::InvalidParameters;
        return result;
    }

    result.seed = source.seed;
    result.nominalDurationMs = source.durationMs;
    result.sections.reserve(source.sections.size());
    std::vector<WorkoutGameSection> adaptedSections;
    adaptedSections.reserve(source.sections.size());
    std::vector<bool> sourceRecoveries;
    sourceRecoveries.reserve(source.sections.size());
    for (std::size_t index = 0; index < source.sections.size(); ++index) {
        const WorkoutGameInterval &interval = intervals[index];
        adaptedSections.push_back(adaptSection(
            source.sections[index], interval, ftpWatts, parameters,
            index == 0, index + 1 == source.sections.size()));
        sourceRecoveries.push_back(
                averageWatts(interval) / ftpWatts
                    <= parameters.recoveryIntensity);
    }
    const WorkoutGameCoursePreset terrainPreset = parameters.technicality <= 0.25
            ? WorkoutGameCoursePreset::WorkoutFirst
            : parameters.technicality >= 0.85
                ? WorkoutGameCoursePreset::RideFirst
                : WorkoutGameCoursePreset::Balanced;
    for (std::size_t index = 0; index < source.sections.size(); ++index) {
        const WorkoutGameInterval &interval = intervals[index];
        WorkoutGameSection adapted = adaptedSections[index];
        const bool sourceRecovery = averageWatts(interval) / ftpWatts <= 0.65;
        const WorkoutGameRoadPhysicsSnapshot before = physics.update({}, 0);

        WorkoutGameDistanceCourseSection section;
        section.feature = adapted.feature;
        section.terrain = adapted.terrain;
        section.sourceStartMs = interval.startMs;
        section.nominalDurationMs = interval.durationMs;
        section.minimumDurationMs = scaledDuration(
                interval.durationMs, sourceRecovery
                    ? parameters.recoveryMinimumDurationScale
                    : parameters.workMinimumDurationScale);
        section.maximumDurationMs = scaledDuration(
                interval.durationMs, sourceRecovery
                    ? parameters.recoveryMaximumDurationScale
                    : parameters.workMaximumDurationScale);
        section.startDistanceMeters = before.distanceMeters;
        section.startElevationMeters = before.elevationMeters;
        section.targetStartWatts = interval.startWatts;
        section.targetEndWatts = interval.endWatts;
        section.gradePercent = adapted.gradePercent;
        section.difficulty = adapted.difficulty;
        section.visualVariant = adapted.visualVariant;
        section.adjustableConnector = adapted.feature
                == WorkoutGameFeature::WarmupTrail
                || isRecoveryFeature(adapted.feature);

        std::int64_t simulatedMs = 0;
        WorkoutGameRoadPhysicsSnapshot after = before;
        while (simulatedMs < interval.durationMs) {
            const std::int64_t stepMs = std::min(
                    parameters.simulationStepMs,
                    interval.durationMs - simulatedMs);
            const double progress = (double(simulatedMs) + stepMs * 0.5)
                    / double(interval.durationMs);
            after = physics.update({
                interval.startWatts
                    + (interval.endWatts - interval.startWatts) * progress,
                adapted.gradePercent,
                0.0
            }, stepMs);
            simulatedMs += stepMs;
        }

        section.lengthMeters = after.distanceMeters
                - section.startDistanceMeters;
        section.endElevationMeters = after.elevationMeters;
        if (!std::isfinite(section.lengthMeters)
                || section.lengthMeters < MinimumSectionLengthMeters) {
            result.status = WorkoutGameDistanceCourseStatus::NoProgress;
            result.sections.clear();
            return result;
        }
        const double elevationChange = section.endElevationMeters
                - section.startElevationMeters;
        result.elevationGainMeters += std::max(0.0, elevationChange);
        result.elevationLossMeters += std::max(0.0, -elevationChange);
        result.sections.push_back(section);
    }

    const double generatedDistanceMeters = result.sections.empty() ? 0.0
            : result.sections.back().startDistanceMeters
                + result.sections.back().lengthMeters;
    // Reserve enough continuous route for effort-semantic geometry before
    // sampling the terrain curve. Reservation does not itself enable an
    // obstacle or bypass the preset's technical-density selection.
    std::vector<bool> featureGeometryReservations(
            result.sections.size(), false);
    for (std::size_t index = 0; index < result.sections.size(); ++index) {
        featureGeometryReservations[index] = !sourceRecoveries[index]
                && effortSemanticsAt(result, index, ftpWatts).hasFeature();
    }
    if (!subdivideLongSections(
                result.sections, adaptedSections, sourceRecoveries,
                featureGeometryReservations,
                parameters.variationLengthMeters,
                parameters.maximumSections)) {
        result.status = WorkoutGameDistanceCourseStatus::ResourceLimit;
        result.sections.clear();
        return result;
    }
    applyTerrainEffortProfile(result, ftpWatts, parameters);
    const bool showcaseCandidate =
            generatedDistanceMeters >= FeatureRichCourseMinimumDistanceMeters
            && std::any_of(
                sourceRecoveries.begin(), sourceRecoveries.end(),
                [](bool recovery) { return !recovery; })
            && std::count_if(
                adaptedSections.begin(), adaptedSections.end(),
                [](const WorkoutGameSection &section) {
                    return WorkoutGameCourseTerrain::showcaseEligible(
                            section.feature);
                }) >= 10;
    std::vector<double> eligibleDistances;
    eligibleDistances.reserve(result.sections.size());
    for (std::size_t index = 0; index < result.sections.size(); ++index) {
        const bool sourceRecovery = sourceRecoveries[index];
        const bool eligible = showcaseCandidate
                ? WorkoutGameCourseTerrain::showcaseEligible(
                    adaptedSections[index].feature)
                : !sourceRecovery && WorkoutGameCourseTerrain::paletteEligible(
                    adaptedSections[index].feature);
        if (eligible) {
            eligibleDistances.push_back(result.sections[index].lengthMeters);
        }
    }
    std::vector<WorkoutGameCourseTerrainSelection> selections =
            WorkoutGameCourseTerrain::selectTechnicalTerrain(
                eligibleDistances, terrainPreset, source.seed);
    if (selections.size() != eligibleDistances.size()) {
        result.status = WorkoutGameDistanceCourseStatus::InvalidParameters;
        result.sections.clear();
        return result;
    }
    if (showcaseCandidate) distributeShowcaseSelections(selections);
    std::size_t paletteIndex = 0u;
    for (std::size_t index = 0; index < result.sections.size(); ++index) {
        const bool sourceRecovery = sourceRecoveries[index];
        const bool paletteEligible = showcaseCandidate
                ? WorkoutGameCourseTerrain::showcaseEligible(
                    adaptedSections[index].feature)
                : !sourceRecovery && WorkoutGameCourseTerrain::paletteEligible(
                    adaptedSections[index].feature);
        const WorkoutGameCourseTerrainSelection selection = paletteEligible
                ? selections[paletteIndex++]
                : WorkoutGameCourseTerrainSelection();
        WorkoutGameCourseTerrain::apply(
                adaptedSections[index], terrainPreset, selection,
                source.seed, sourceRecovery);
        alignTechnicalTerrainWithEffort(
                adaptedSections[index], result, index, ftpWatts,
                terrainPreset, selection, sourceRecovery);
        result.sections[index].feature = adaptedSections[index].feature;
        result.sections[index].terrain = adaptedSections[index].terrain;
        result.sections[index].challengeCount =
                adaptedSections[index].challengeCount;
    }

    result.totalDistanceMeters = result.sections.back().startDistanceMeters
            + result.sections.back().lengthMeters;
    return result;
}

WorkoutGameDistanceCourseEstimate WorkoutGameDistanceCourseEstimator::estimate(
        const WorkoutGameDistanceCourse &course,
        const WorkoutGameRoadPhysicsParameters &physicsParameters,
        double rawPowerScale,
        std::int64_t rawSimulationStepMs)
{
    WorkoutGameDistanceCourseEstimate result;
    if (!WorkoutGameDistanceCourseBuilder::validCourse(course)
            || !WorkoutGameRoadPhysics::validParameters(physicsParameters)
            || !std::isfinite(rawPowerScale)
            || rawPowerScale < 0.0
            || rawPowerScale > 3.0
            || rawSimulationStepMs < 10
            || rawSimulationStepMs > 1000) {
        return result;
    }

    WorkoutGameRoadPhysics physics;
    if (!physics.configure(physicsParameters)) return result;
    const std::int64_t scaledMaximum = course.nominalDurationMs
            > MaximumEstimateDurationMs / 3
            ? MaximumEstimateDurationMs
            : course.nominalDurationMs * 3;
    const std::int64_t maximumDurationMs = std::clamp<std::int64_t>(
            scaledMaximum, 60000, MaximumEstimateDurationMs);
    std::size_t sectionIndex = 0;
    double progressDistanceMeters = 0.0;
    while (result.elapsedTimeMs < maximumDurationMs) {
        const WorkoutGameDistanceCourseSection &section =
                course.sections[sectionIndex];
        const std::int64_t stepMs = std::min(
                rawSimulationStepMs,
                maximumDurationMs - result.elapsedTimeMs);
        const double distanceProgress = std::clamp(
                (progressDistanceMeters - section.startDistanceMeters)
                    / section.lengthMeters,
                0.0, 1.0);
        const double targetProgress = std::min(
                1.0, distanceProgress + std::max(
                    0.01, 1.0 / section.lengthMeters));
        const double targetWatts =
                referenceEffortAt(section, targetProgress) * rawPowerScale;
        const WorkoutGameRoadPhysicsSnapshot after = physics.update({
            targetWatts,
            section.gradePercent,
            0.0
        }, stepMs);
        result.elapsedTimeMs += stepMs;
        progressDistanceMeters = std::min(
                after.distanceMeters, course.totalDistanceMeters);
        constexpr double BoundaryEpsilonMeters = 1.0e-9;
        while (sectionIndex + 1 < course.sections.size()
                && progressDistanceMeters
                    >= course.sections[sectionIndex].startDistanceMeters
                        + course.sections[sectionIndex].lengthMeters
                        - BoundaryEpsilonMeters) {
            ++sectionIndex;
        }
        result.distanceMeters = progressDistanceMeters;
        const WorkoutGameDistanceCourseSection &positionSection =
                course.sections[sectionIndex];
        const double sectionProgress = std::clamp(
                (progressDistanceMeters - positionSection.startDistanceMeters)
                    / positionSection.lengthMeters,
                0.0, 1.0);
        result.elevationMeters = positionSection.startElevationMeters
                + (positionSection.endElevationMeters
                    - positionSection.startElevationMeters)
                    * sectionProgress;
        if (progressDistanceMeters >= course.totalDistanceMeters) {
            result.finished = true;
            return result;
        }
    }
    return result;
}
