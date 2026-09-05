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
    latestProgress = playback.atDistance(0.0);
    configured = true;
    return WorkoutGameCourseRuntimeStatus::Ready;
}

void WorkoutGameCourseRuntime::reset()
{
    configured = false;
    configuredFtpWatts = 0.0;
    configuredVisualCourse = WorkoutGameCourse();
    playback = WorkoutGameDistancePlayback();
    latestProgress = WorkoutGameDistancePlaybackSnapshot();
}

void WorkoutGameCourseRuntime::restartProgress()
{
    playback.resetProgress();
    latestProgress = configured ? playback.atDistance(0.0)
                                : WorkoutGameDistancePlaybackSnapshot();
}

bool WorkoutGameCourseRuntime::enabled() const
{
    return configured;
}

double WorkoutGameCourseRuntime::ftpWatts() const
{
    return configuredFtpWatts;
}

double WorkoutGameCourseRuntime::workoutTimelinePositionMeters() const
{
    return configured && latestProgress.ready
            ? latestProgress.timelineDistanceMeters : 0.0;
}

const WorkoutGameCourse &WorkoutGameCourseRuntime::visualCourse() const
{
    return configuredVisualCourse;
}

WorkoutGameDistancePlaybackSnapshot
WorkoutGameCourseRuntime::atWorkoutPosition(
        double positionMeters) const
{
    if (!configured) return {};
    return playback.atDistance(positionMeters);
}

WorkoutGameDistancePlaybackSnapshot
WorkoutGameCourseRuntime::atWorkoutProgress(
        double rawPositionMeters,
        std::int64_t elapsedTimeMs,
        bool moving)
{
    if (!configured) return {};
    latestProgress = playback.atProgress(
            rawPositionMeters, elapsedTimeMs, moving);
    return latestProgress;
}

WorkoutGameDistancePlaybackSnapshot
WorkoutGameCourseRuntime::seekToWorkoutPosition(
        double positionMeters,
        std::int64_t elapsedTimeMs)
{
    if (!configured) return {};
    latestProgress = playback.seekToDistance(positionMeters, elapsedTimeMs);
    return latestProgress;
}

double WorkoutGameCourseRuntime::generatedTargetWattsAt(
        double positionMeters, double relativeGearRatio) const
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

double WorkoutGameCourseRuntime::generatedProgressTargetWatts(
        double relativeGearRatio) const
{
    if (!configured || !latestProgress.ready
            || !std::isfinite(latestProgress.targetWatts)
            || !std::isfinite(relativeGearRatio)
            || relativeGearRatio <= 0.0) {
        return -1.0;
    }
    return std::clamp(
            latestProgress.targetWatts * relativeGearRatio, 0.0, 2500.0);
}
