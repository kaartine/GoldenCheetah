/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCourseRuntime_h
#define _GC_WorkoutGameCourseRuntime_h

#include "WorkoutGameDistancePlayback.h"

#include <QString>

#include <cstdint>

enum class WorkoutGameCourseRuntimeStatus
{
    Ready,
    MetadataUnavailable,
    InvalidMetadata
};

class WorkoutGameCourseRuntime
{
public:
    WorkoutGameCourseRuntimeStatus configure(const QString &coursePath);
    void reset();

    bool enabled() const;
    double ftpWatts() const;
    const WorkoutGameCourse &visualCourse() const;
    WorkoutGameDistancePlaybackSnapshot atWorkoutPosition(
            std::int64_t positionMeters) const;

private:
    bool configured = false;
    double configuredFtpWatts = 0.0;
    WorkoutGameCourse configuredVisualCourse;
    WorkoutGameDistancePlayback playback;
};

#endif
