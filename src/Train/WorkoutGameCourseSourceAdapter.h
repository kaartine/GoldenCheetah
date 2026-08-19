/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCourseSourceAdapter_h
#define _GC_WorkoutGameCourseSourceAdapter_h

#include "WorkoutGameCourseDocument.h"
#include "WorkoutGameWorkoutAdapter.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <vector>

enum class WorkoutGameCourseSourceStatus
{
    Ready,
    InvalidWorkout,
    InvalidFtp,
    InvalidSource,
    ConversionFailed
};

struct WorkoutGameCourseSourceRequest
{
    std::vector<WorkoutGamePowerPoint> points;
    QByteArray sourceContents;
    QString sourceFileName;
    QString title;
    double ftpWatts = 0.0;
    WorkoutGameCoursePreset preset = WorkoutGameCoursePreset::Balanced;
    WorkoutGameRoadPhysicsParameters roadPhysics;
    std::uint32_t seed = 0;
};

struct WorkoutGameCourseSourceResult
{
    WorkoutGameCourseSourceStatus status =
            WorkoutGameCourseSourceStatus::InvalidSource;
    WorkoutGameCourseDocument document;
    WorkoutGameCourseConversionSummary summary;
};

class WorkoutGameCourseSourceAdapter
{
public:
    static constexpr qsizetype MaximumSourceBytes = 64 * 1024 * 1024;

    static WorkoutGameCourseSourceResult convert(
            const WorkoutGameCourseSourceRequest &request);
};

#endif
