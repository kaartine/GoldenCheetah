/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameDistancePlayback_h
#define _GC_WorkoutGameDistancePlayback_h

#include "WorkoutGameDistanceCourse.h"

#include <cstddef>
#include <cstdint>

struct WorkoutGameDistancePlaybackSnapshot
{
    bool ready = false;
    bool finished = false;
    bool maximumExposureExceeded = false;
    std::size_t sectionIndex = 0;
    std::int64_t nominalTimeMs = 0;
    std::int64_t sectionElapsedMs = 0;
    double distanceMeters = 0.0;
    double sectionProgress = 0.0;
    double targetWatts = 0.0;
    double gradePercent = 0.0;
    WorkoutGameFeature feature = WorkoutGameFeature::Trail;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
};

class WorkoutGameDistancePlayback
{
public:
    bool configure(const WorkoutGameDistanceCourse &course);
    void resetProgress();
    WorkoutGameDistancePlaybackSnapshot atDistance(double distanceMeters) const;
    WorkoutGameDistancePlaybackSnapshot atProgress(
            double rawDistanceMeters,
            std::int64_t elapsedTimeMs,
            bool moving = true);
    WorkoutGameDistancePlaybackSnapshot seekToDistance(
            double distanceMeters,
            std::int64_t elapsedTimeMs);

    static WorkoutGameCourse visualCourse(
            const WorkoutGameDistanceCourse &course);

private:
    WorkoutGameDistancePlaybackSnapshot progressSnapshot() const;
    void anchorProgress(double distanceMeters, std::int64_t elapsedTimeMs);

    WorkoutGameDistanceCourse configuredCourse;
    double lastRawDistanceMeters = 0.0;
    double progressDistanceMeters = 0.0;
    std::size_t progressSectionIndex = 0;
    std::int64_t progressSectionActiveTimeMs = 0;
    std::int64_t lastProgressTimeMs = 0;
};

#endif
