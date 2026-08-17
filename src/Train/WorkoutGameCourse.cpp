/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCourse.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr std::uint32_t FnvOffset = 2166136261u;
constexpr std::uint32_t FnvPrime = 16777619u;

void hashValue(std::uint32_t &hash, std::uint64_t value)
{
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= std::uint8_t(value & 0xffu);
        hash *= FnvPrime;
        value >>= 8;
    }
}

std::uint32_t derivedSeed(
        const std::vector<WorkoutGameInterval> &intervals,
        double ftpWatts)
{
    std::uint32_t hash = FnvOffset;
    hashValue(hash, std::uint64_t(std::llround(ftpWatts * 10.0)));
    for (const WorkoutGameInterval &interval : intervals) {
        hashValue(hash, std::uint64_t(interval.startMs));
        hashValue(hash, std::uint64_t(interval.durationMs));
        hashValue(hash, std::uint64_t(std::llround(interval.startWatts * 10.0)));
        hashValue(hash, std::uint64_t(std::llround(interval.endWatts * 10.0)));
    }
    return hash == 0 ? FnvOffset : hash;
}

std::uint32_t nextRandom(std::uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

WorkoutGameFeature classify(
        const WorkoutGameInterval &interval,
        double ftpWatts,
        bool first,
        bool last)
{
    const double averageWatts = (interval.startWatts + interval.endWatts) / 2.0;
    const double intensity = averageWatts / ftpWatts;
    const double rise = (interval.endWatts - interval.startWatts) / ftpWatts;

    if (first
            && interval.durationMs >= 60000
            && rise >= 0.1
            && intensity < 0.9) {
        return WorkoutGameFeature::WarmupTrail;
    }
    if (last
            && interval.durationMs >= 60000
            && rise <= -0.1
            && intensity < 0.8) {
        return WorkoutGameFeature::CooldownDescent;
    }
    if (intensity <= 0.6) return WorkoutGameFeature::RecoveryDescent;
    if (interval.durationMs <= 20000 && intensity >= 1.1) {
        return WorkoutGameFeature::SprintJump;
    }
    if (rise >= 0.15
            || (interval.durationMs >= 180000 && intensity >= 0.88)) {
        return WorkoutGameFeature::Climb;
    }
    if (interval.durationMs >= 45000 && intensity >= 0.78) {
        return WorkoutGameFeature::FlowTrail;
    }
    return WorkoutGameFeature::Trail;
}

WorkoutGameSection makeSection(
        const WorkoutGameInterval &interval,
        double ftpWatts,
        WorkoutGameFeature feature,
        std::uint32_t &randomState)
{
    WorkoutGameSection section;
    section.feature = feature;
    section.startMs = interval.startMs;
    section.durationMs = interval.durationMs;
    section.targetWatts = (interval.startWatts + interval.endWatts) / 2.0;
    section.difficulty = std::clamp(
            (section.targetWatts / ftpWatts - 0.5) / 0.8, 0.0, 1.0);
    section.visualVariant = nextRandom(randomState) % 8u;

    switch (feature) {
    case WorkoutGameFeature::WarmupTrail:
        section.gradePercent = 1.0 + 2.0 * section.difficulty;
        break;
    case WorkoutGameFeature::Trail:
        section.gradePercent = 0.5 * section.difficulty;
        break;
    case WorkoutGameFeature::FlowTrail:
        section.gradePercent = 1.0 + 2.0 * section.difficulty;
        section.challengeCount = std::max(1, int(interval.durationMs / 30000));
        break;
    case WorkoutGameFeature::Climb:
        section.gradePercent = 3.0 + 5.0 * section.difficulty;
        section.challengeCount = std::max(1, int(interval.durationMs / 60000));
        break;
    case WorkoutGameFeature::SprintJump:
        section.gradePercent = 2.0;
        section.challengeCount = 1;
        break;
    case WorkoutGameFeature::RecoveryDescent:
        section.gradePercent = -4.0;
        section.gravityAssisted = true;
        break;
    case WorkoutGameFeature::CooldownDescent:
        section.gradePercent = -3.0;
        section.gravityAssisted = true;
        break;
    }
    return section;
}

}

WorkoutGameCourse WorkoutGameCourseBuilder::build(
        const std::vector<WorkoutGameInterval> &intervals,
        double ftpWatts,
        std::uint32_t requestedSeed)
{
    WorkoutGameCourse course;
    if (!std::isfinite(ftpWatts) || ftpWatts <= 0.0) {
        course.status = WorkoutGameCourseStatus::InvalidFtp;
        return course;
    }
    if (intervals.empty()) return course;

    std::int64_t expectedStart = 0;
    for (const WorkoutGameInterval &interval : intervals) {
        if (interval.startMs != expectedStart
                || interval.durationMs <= 0
                || !std::isfinite(interval.startWatts)
                || !std::isfinite(interval.endWatts)
                || interval.startWatts < 0.0
                || interval.endWatts < 0.0
                || interval.durationMs >
                        std::numeric_limits<std::int64_t>::max() - expectedStart) {
            course.status = WorkoutGameCourseStatus::InvalidInterval;
            return course;
        }
        expectedStart += interval.durationMs;
    }

    course.status = WorkoutGameCourseStatus::Ready;
    course.durationMs = expectedStart;
    course.seed = requestedSeed != 0
            ? requestedSeed
            : derivedSeed(intervals, ftpWatts);
    std::uint32_t randomState = course.seed;
    course.sections.reserve(intervals.size());
    for (std::size_t index = 0; index < intervals.size(); ++index) {
        const WorkoutGameFeature feature = classify(
                intervals[index],
                ftpWatts,
                index == 0,
                index + 1 == intervals.size());
        course.sections.push_back(makeSection(
                intervals[index], ftpWatts, feature, randomState));
    }
    return course;
}
