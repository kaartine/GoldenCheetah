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
    configuredPreset = document.preset;
    latestProgress = playback.atDistance(0.0);
    configured = true;
    return WorkoutGameCourseRuntimeStatus::Ready;
}

void WorkoutGameCourseRuntime::reset()
{
    configured = false;
    configuredFtpWatts = 0.0;
    configuredPreset = WorkoutGameCoursePreset::Balanced;
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

WorkoutGameCoursePreset WorkoutGameCourseRuntime::coursePreset() const
{
    return configuredPreset;
}

double WorkoutGameCourseRuntime::workoutTimelinePositionMeters() const
{
    return configured && latestProgress.ready
            ? latestProgress.timelineDistanceMeters : 0.0;
}

std::int64_t WorkoutGameCourseRuntime::workoutTimelinePositionMs() const
{
    return configured && latestProgress.ready
            ? latestProgress.nominalTimeMs : 0;
}

std::int64_t WorkoutGameCourseRuntime::generatedProgressSectionDurationMs() const
{
    return configured && latestProgress.ready
            ? latestProgress.sectionDurationMs : 0;
}

double WorkoutGameCourseRuntime::generatedProgressSectionProgress() const
{
    return configured && latestProgress.ready
            && std::isfinite(latestProgress.sectionProgress)
            ? latestProgress.sectionProgress : 0.0;
}

WorkoutGameTerrainKind WorkoutGameCourseRuntime::generatedProgressTerrain() const
{
    return configured && latestProgress.ready
            ? latestProgress.terrain : WorkoutGameTerrainKind::SmoothTrail;
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
