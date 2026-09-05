/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameDistancePlayback.h"
#include "WorkoutGameFeatureCatalog.h"

#include <algorithm>
#include <cmath>

bool WorkoutGameDistancePlayback::configure(
        const WorkoutGameDistanceCourse &course)
{
    configuredCourse = WorkoutGameDistanceCourse();
    resetProgress();
    if (!WorkoutGameDistanceCourseBuilder::validCourse(course)) return false;
    configuredCourse = course;
    return true;
}

void WorkoutGameDistancePlayback::resetProgress()
{
    lastRawDistanceMeters = 0.0;
    progressDistanceMeters = 0.0;
    progressSectionIndex = 0;
    progressSectionActiveTimeMs = 0;
    lastProgressTimeMs = 0;
    maximumExposureExceededOnLastAdvance = false;
}

WorkoutGameDistancePlaybackSnapshot WorkoutGameDistancePlayback::atDistance(
        double rawDistanceMeters) const
{
    WorkoutGameDistancePlaybackSnapshot result;
    if (!std::isfinite(rawDistanceMeters)
            || !WorkoutGameDistanceCourseBuilder::validCourse(configuredCourse)) {
        return result;
    }

    result.ready = true;
    result.distanceMeters = std::clamp(
            rawDistanceMeters, 0.0, configuredCourse.totalDistanceMeters);
    result.finished = result.distanceMeters >= configuredCourse.totalDistanceMeters;

    auto section = std::upper_bound(
            configuredCourse.sections.begin(),
            configuredCourse.sections.end(),
            result.distanceMeters,
            [](double distance, const WorkoutGameDistanceCourseSection &candidate) {
                return distance < candidate.startDistanceMeters;
            });
    if (section != configuredCourse.sections.begin()) --section;
    if (result.finished) section = configuredCourse.sections.end() - 1;
    result.sectionIndex = std::size_t(
            std::distance(configuredCourse.sections.begin(), section));
    result.sectionProgress = section->lengthMeters > 0.0
            ? std::clamp(
                (result.distanceMeters - section->startDistanceMeters)
                    / section->lengthMeters,
                0.0, 1.0)
            : 1.0;
    result.nominalTimeMs = result.finished
            ? configuredCourse.nominalDurationMs
            : section->sourceStartMs + std::int64_t(std::llround(
                result.sectionProgress * double(section->nominalDurationMs)));
    result.timelineDistanceMeters = distanceAtNominalTime(
            configuredCourse, result.nominalTimeMs);
    result.targetWatts = section->targetStartWatts
            + (section->targetEndWatts - section->targetStartWatts)
                * result.sectionProgress;
    result.gradePercent = section->gradePercent;
    result.feature = section->feature;
    result.terrain = section->terrain;
    return result;
}

WorkoutGameDistancePlaybackSnapshot WorkoutGameDistancePlayback::atProgress(
        double rawDistanceMeters,
        std::int64_t elapsedTimeMs,
        bool moving)
{
    if (!std::isfinite(rawDistanceMeters) || elapsedTimeMs < 0
            || elapsedTimeMs < lastProgressTimeMs
            || !WorkoutGameDistanceCourseBuilder::validCourse(configuredCourse)) {
        return {};
    }

    const double rawDistance = std::max(0.0, rawDistanceMeters);
    maximumExposureExceededOnLastAdvance = false;
    if (rawDistance < lastRawDistanceMeters) {
        lastRawDistanceMeters = rawDistance;
        lastProgressTimeMs = elapsedTimeMs;
        return progressSnapshot();
    }

    const double rawAdvance = rawDistance - lastRawDistanceMeters;
    const std::int64_t elapsedAdvanceMs = elapsedTimeMs - lastProgressTimeMs;
    lastRawDistanceMeters = rawDistance;
    lastProgressTimeMs = elapsedTimeMs;
    if (progressDistanceMeters >= configuredCourse.totalDistanceMeters) {
        return progressSnapshot();
    }
    if (!moving) return progressSnapshot();

    const WorkoutGameDistanceCourseSection &section =
            configuredCourse.sections[progressSectionIndex];
    const double sectionEnd = section.startDistanceMeters
            + section.lengthMeters;
    bool reachedMaximumExposure = false;
    std::int64_t carriedActiveTimeMs = 0;
    if (rawAdvance > 1.0e-9) {
        const std::int64_t timeUntilMaximum =
                section.maximumDurationMs - progressSectionActiveTimeMs;
        if (elapsedAdvanceMs >= timeUntilMaximum) {
            progressSectionActiveTimeMs = section.maximumDurationMs;
            carriedActiveTimeMs = elapsedAdvanceMs - timeUntilMaximum;
            reachedMaximumExposure = true;
        } else {
            progressSectionActiveTimeMs += elapsedAdvanceMs;
        }
    }
    const double timeProgress = std::clamp(
            double(progressSectionActiveTimeMs)
                / double(section.minimumDurationMs),
            0.0, 1.0);
    const double timeBound = section.startDistanceMeters
            + section.lengthMeters * timeProgress;
    const double availableAdvance = std::max(
            0.0, std::min(sectionEnd, timeBound) - progressDistanceMeters);
    progressDistanceMeters += std::min(rawAdvance, availableAdvance);

    constexpr double BoundaryEpsilonMeters = 1.0e-9;
    const bool sectionBoundaryReached =
            progressDistanceMeters >= sectionEnd - BoundaryEpsilonMeters;
    if (sectionBoundaryReached || reachedMaximumExposure) {
        progressDistanceMeters = sectionEnd;
        maximumExposureExceededOnLastAdvance =
                reachedMaximumExposure && carriedActiveTimeMs > 0;
        while (progressSectionIndex + 1 < configuredCourse.sections.size()) {
            ++progressSectionIndex;
            if (!reachedMaximumExposure) {
                progressSectionActiveTimeMs = 0;
                break;
            }
            const WorkoutGameDistanceCourseSection &nextSection =
                    configuredCourse.sections[progressSectionIndex];
            if (carriedActiveTimeMs < nextSection.maximumDurationMs) {
                progressSectionActiveTimeMs = carriedActiveTimeMs;
                break;
            }
            carriedActiveTimeMs -= nextSection.maximumDurationMs;
            progressSectionActiveTimeMs = nextSection.maximumDurationMs;
            progressDistanceMeters = nextSection.startDistanceMeters
                    + nextSection.lengthMeters;
        }
    }
    return progressSnapshot();
}

WorkoutGameDistancePlaybackSnapshot WorkoutGameDistancePlayback::seekToDistance(
        double distanceMeters,
        std::int64_t elapsedTimeMs)
{
    if (!std::isfinite(distanceMeters) || elapsedTimeMs < 0
            || !WorkoutGameDistanceCourseBuilder::validCourse(configuredCourse)) {
        return {};
    }
    anchorProgress(distanceMeters, elapsedTimeMs);
    return progressSnapshot();
}

void WorkoutGameDistancePlayback::anchorProgress(
        double distanceMeters,
        std::int64_t elapsedTimeMs)
{
    progressDistanceMeters = std::clamp(
            distanceMeters, 0.0, configuredCourse.totalDistanceMeters);
    lastRawDistanceMeters = progressDistanceMeters;
    lastProgressTimeMs = elapsedTimeMs;
    const WorkoutGameDistancePlaybackSnapshot snapshot =
            atDistance(progressDistanceMeters);
    progressSectionIndex = snapshot.sectionIndex;
    maximumExposureExceededOnLastAdvance = false;
    const WorkoutGameDistanceCourseSection &section =
            configuredCourse.sections[progressSectionIndex];
    progressSectionActiveTimeMs = std::int64_t(std::llround(
            snapshot.sectionProgress * double(section.minimumDurationMs)));
}

WorkoutGameDistancePlaybackSnapshot
WorkoutGameDistancePlayback::progressSnapshot() const
{
    WorkoutGameDistancePlaybackSnapshot result =
            atDistance(progressDistanceMeters);
    if (!result.ready) return result;
    result.sectionElapsedMs = progressSectionActiveTimeMs;
    const WorkoutGameDistanceCourseSection &section =
            configuredCourse.sections[result.sectionIndex];
    const double targetProgress = result.finished
            ? 1.0
            : std::clamp(
                double(progressSectionActiveTimeMs)
                    / double(section.nominalDurationMs),
                0.0, 1.0);
    result.nominalTimeMs = result.finished
            ? configuredCourse.nominalDurationMs
            : section.sourceStartMs + std::int64_t(std::llround(
                targetProgress * double(section.nominalDurationMs)));
    result.timelineDistanceMeters = distanceAtNominalTime(
            configuredCourse, result.nominalTimeMs);
    result.targetWatts = result.finished
            ? 0.0
            : section.targetStartWatts
                + (section.targetEndWatts - section.targetStartWatts)
                    * targetProgress;
    result.maximumExposureExceeded = maximumExposureExceededOnLastAdvance;
    return result;
}

double WorkoutGameDistancePlayback::distanceAtNominalTime(
        const WorkoutGameDistanceCourse &course,
        std::int64_t nominalTimeMs)
{
    if (course.status != WorkoutGameDistanceCourseStatus::Ready
            || course.sections.empty()
            || course.nominalDurationMs <= 0
            || !std::isfinite(course.totalDistanceMeters)
            || course.totalDistanceMeters <= 0.0
            || nominalTimeMs <= 0) {
        return 0.0;
    }
    if (nominalTimeMs >= course.nominalDurationMs) {
        return course.totalDistanceMeters;
    }
    for (const WorkoutGameDistanceCourseSection &section : course.sections) {
        const std::int64_t sectionEndMs =
                section.sourceStartMs + section.nominalDurationMs;
        if (nominalTimeMs <= sectionEndMs) {
            const double progress = section.nominalDurationMs > 0
                    ? std::clamp(
                        double(nominalTimeMs - section.sourceStartMs)
                            / double(section.nominalDurationMs),
                        0.0, 1.0)
                    : 0.0;
            return section.startDistanceMeters
                    + section.lengthMeters * progress;
        }
    }
    return course.totalDistanceMeters;
}

WorkoutGameCourse WorkoutGameDistancePlayback::visualCourse(
        const WorkoutGameDistanceCourse &course)
{
    WorkoutGameCourse result;
    if (!WorkoutGameDistanceCourseBuilder::validCourse(course)) return result;

    result.status = WorkoutGameCourseStatus::Ready;
    result.seed = course.seed;
    result.durationMs = course.nominalDurationMs;
    result.roadPlan = course.roadPlan;
    result.sections.reserve(course.sections.size());
    for (const WorkoutGameDistanceCourseSection &source : course.sections) {
        WorkoutGameSection section;
        section.feature = source.feature;
        section.terrain = source.terrain;
        section.startMs = source.sourceStartMs;
        section.durationMs = source.nominalDurationMs;
        section.targetWatts = (source.targetStartWatts
                + source.targetEndWatts) * 0.5;
        section.gradePercent = source.gradePercent;
        section.lengthMeters = source.lengthMeters;
        section.difficulty = source.difficulty;
        const bool recovery = source.feature
                    == WorkoutGameFeature::RecoveryDescent
                || source.feature == WorkoutGameFeature::CooldownDescent;
        section.challengeCount = !recovery
                && (source.feature != WorkoutGameFeature::Trail
                    || WorkoutGameFeatureCatalog::definition(
                        source.terrain).technical)
                ? 1 : 0;
        section.visualVariant = source.visualVariant;
        section.gravityAssisted = source.feature
                == WorkoutGameFeature::RecoveryDescent
                || source.feature == WorkoutGameFeature::CooldownDescent;
        result.sections.push_back(section);
    }
    return result;
}
