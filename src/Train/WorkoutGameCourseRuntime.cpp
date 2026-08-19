/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCourseRuntime.h"

#include "WorkoutGameCourseDocument.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>

WorkoutGameCourseRuntimeStatus WorkoutGameCourseRuntime::configure(
        const QString &coursePath)
{
    reset();
    const QString sidecar =
            WorkoutGameCourseDocumentStore::sidecarPathForCourse(coursePath);
    if (!QFileInfo(sidecar).isFile()) {
        return WorkoutGameCourseRuntimeStatus::MetadataUnavailable;
    }

    WorkoutGameCourseDocument document;
    QString error;
    if (WorkoutGameCourseDocumentStore::loadForCourse(
                coursePath, document, error)
            != WorkoutGameCourseDocumentStatus::Ready
            || !playback.configure(document.course)) {
        reset();
        return WorkoutGameCourseRuntimeStatus::InvalidMetadata;
    }
    configuredVisualCourse =
            WorkoutGameDistancePlayback::visualCourse(document.course);
    if (configuredVisualCourse.status != WorkoutGameCourseStatus::Ready) {
        reset();
        return WorkoutGameCourseRuntimeStatus::InvalidMetadata;
    }
    configuredFtpWatts = document.ftpWatts;
    configured = true;
    return WorkoutGameCourseRuntimeStatus::Ready;
}

void WorkoutGameCourseRuntime::reset()
{
    configured = false;
    configuredFtpWatts = 0.0;
    configuredVisualCourse = WorkoutGameCourse();
    playback = WorkoutGameDistancePlayback();
}

bool WorkoutGameCourseRuntime::enabled() const
{
    return configured;
}

double WorkoutGameCourseRuntime::ftpWatts() const
{
    return configuredFtpWatts;
}

const WorkoutGameCourse &WorkoutGameCourseRuntime::visualCourse() const
{
    return configuredVisualCourse;
}

WorkoutGameDistancePlaybackSnapshot
WorkoutGameCourseRuntime::atWorkoutPosition(
        std::int64_t positionMeters) const
{
    if (!configured) return {};
    return playback.atDistance(double(positionMeters));
}

double WorkoutGameCourseRuntime::generatedTargetWattsAt(
        std::int64_t positionMeters, double relativeGearRatio) const
{
    if (!configured || !std::isfinite(relativeGearRatio)
            || relativeGearRatio <= 0.0) {
        return -1.0;
    }

    const WorkoutGameDistancePlaybackSnapshot snapshot =
            atWorkoutPosition(positionMeters);
    if (!snapshot.ready || !std::isfinite(snapshot.targetWatts)) return -1.0;

    return std::clamp(snapshot.targetWatts * relativeGearRatio, 0.0, 2500.0);
}
