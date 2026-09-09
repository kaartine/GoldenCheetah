/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameDistanceCourse.h"
#include "WorkoutGameCourseTerrain.h"

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

double targetAt(
        const WorkoutGameDistanceCourseSection &section,
        double progress)
{
    const double clamped = std::clamp(progress, 0.0, 1.0);
    return section.targetStartWatts
            + (section.targetEndWatts - section.targetStartWatts) * clamped;
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
        std::size_t maximumSections)
{
    std::size_t required = 0;
    for (const WorkoutGameDistanceCourseSection &section : sections) {
        required += std::size_t(std::max<std::int64_t>(
                1, (section.nominalDurationMs
                    + MaximumVisualSectionDurationMs - 1)
                        / MaximumVisualSectionDurationMs));
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
        const int partCount = int(std::max<std::int64_t>(
                1, (source.nominalDurationMs
                    + MaximumVisualSectionDurationMs - 1)
                        / MaximumVisualSectionDurationMs));
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
            section.targetStartWatts = targetAt(source, startProgress);
            section.targetEndWatts = targetAt(source, endProgress);
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
    if (generatedDistanceMeters >= FeatureRichCourseMinimumDistanceMeters
            && !subdivideLongSections(
                result.sections, adaptedSections, sourceRecoveries,
                parameters.maximumSections)) {
        result.status = WorkoutGameDistanceCourseStatus::ResourceLimit;
        result.sections.clear();
        return result;
    }
    const bool showcaseCandidate = generatedDistanceMeters >= 5000.0
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
    double rawDistanceMeters = 0.0;
    double progressDistanceMeters = 0.0;
    std::int64_t sectionActiveTimeMs = 0;
    while (result.elapsedTimeMs < maximumDurationMs) {
        const WorkoutGameDistanceCourseSection &section =
                course.sections[sectionIndex];
        const std::int64_t stepMs = std::min(
                rawSimulationStepMs,
                maximumDurationMs - result.elapsedTimeMs);
        const double targetProgress = std::clamp(
                (double(sectionActiveTimeMs) + double(stepMs) * 0.5)
                    / double(section.nominalDurationMs),
                0.0, 1.0);
        const double targetWatts =
                targetAt(section, targetProgress) * rawPowerScale;
        const WorkoutGameRoadPhysicsSnapshot after = physics.update({
            targetWatts,
            section.gradePercent,
            0.0
        }, stepMs);
        result.elapsedTimeMs += stepMs;
        const double rawAdvance = std::max(
                0.0, after.distanceMeters - rawDistanceMeters);
        rawDistanceMeters = after.distanceMeters;
        // ETA assumes continuous active riding. Apply the same bounded
        // section exposure as runtime so preview and playback agree.
        const std::int64_t timeUntilMaximum =
                section.maximumDurationMs - sectionActiveTimeMs;
        const bool reachedMaximumExposure = stepMs >= timeUntilMaximum;
        std::int64_t carriedActiveTimeMs = 0;
        if (reachedMaximumExposure) {
            sectionActiveTimeMs = section.maximumDurationMs;
            carriedActiveTimeMs = stepMs - timeUntilMaximum;
        } else {
            sectionActiveTimeMs += stepMs;
        }
        const double timeProgress = std::clamp(
                double(sectionActiveTimeMs)
                    / double(section.minimumDurationMs),
                0.0, 1.0);
        const double sectionEnd = section.startDistanceMeters
                + section.lengthMeters;
        const double timeBound = section.startDistanceMeters
                + section.lengthMeters * timeProgress;
        const double availableAdvance = std::max(
                0.0, std::min(sectionEnd, timeBound)
                    - progressDistanceMeters);
        progressDistanceMeters += std::min(rawAdvance, availableAdvance);
        constexpr double BoundaryEpsilonMeters = 1.0e-9;
        if (progressDistanceMeters >= sectionEnd - BoundaryEpsilonMeters
                || reachedMaximumExposure) {
            progressDistanceMeters = sectionEnd;
            while (sectionIndex + 1 < course.sections.size()) {
                ++sectionIndex;
                if (!reachedMaximumExposure) {
                    sectionActiveTimeMs = 0;
                    break;
                }
                const WorkoutGameDistanceCourseSection &nextSection =
                        course.sections[sectionIndex];
                if (carriedActiveTimeMs < nextSection.maximumDurationMs) {
                    sectionActiveTimeMs = carriedActiveTimeMs;
                    break;
                }
                carriedActiveTimeMs -= nextSection.maximumDurationMs;
                sectionActiveTimeMs = nextSection.maximumDurationMs;
                progressDistanceMeters = nextSection.startDistanceMeters
                        + nextSection.lengthMeters;
            }
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
