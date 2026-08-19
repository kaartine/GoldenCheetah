/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameDistancePlayback.h"

#include <algorithm>
#include <cmath>

bool WorkoutGameDistancePlayback::configure(
        const WorkoutGameDistanceCourse &course)
{
    configuredCourse = WorkoutGameDistanceCourse();
    if (!WorkoutGameDistanceCourseBuilder::validCourse(course)) return false;
    configuredCourse = course;
    return true;
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
    result.targetWatts = section->targetStartWatts
            + (section->targetEndWatts - section->targetStartWatts)
                * result.sectionProgress;
    result.gradePercent = section->gradePercent;
    result.feature = section->feature;
    result.terrain = section->terrain;
    return result;
}

WorkoutGameCourse WorkoutGameDistancePlayback::visualCourse(
        const WorkoutGameDistanceCourse &course)
{
    WorkoutGameCourse result;
    if (!WorkoutGameDistanceCourseBuilder::validCourse(course)) return result;

    result.status = WorkoutGameCourseStatus::Ready;
    result.seed = course.seed;
    result.durationMs = course.nominalDurationMs;
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
        section.difficulty = source.difficulty;
        section.challengeCount = source.feature == WorkoutGameFeature::Trail
                ? 0 : 1;
        section.visualVariant = source.visualVariant;
        section.gravityAssisted = source.feature
                == WorkoutGameFeature::RecoveryDescent
                || source.feature == WorkoutGameFeature::CooldownDescent;
        result.sections.push_back(section);
    }
    return result;
}
