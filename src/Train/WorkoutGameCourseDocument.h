/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCourseDocument_h
#define _GC_WorkoutGameCourseDocument_h

#include "WorkoutGameCourseConversion.h"

#include <QByteArray>
#include <QString>

enum class WorkoutGameCourseDocumentStatus
{
    Ready,
    InvalidDocument,
    InvalidJson,
    UnsupportedVersion,
    ResourceLimit,
    Conflict,
    IoError
};

struct WorkoutGameCourseDocument
{
    int schemaVersion = 2;
    QString title;
    QString sourceFileName;
    QString sourceSha256;
    std::vector<WorkoutGameInterval> sourceIntervals;
    double ftpWatts = 0.0;
    WorkoutGameCoursePreset preset = WorkoutGameCoursePreset::Balanced;
    WorkoutGameDistanceCourseGenerationParameters generationParameters;
    WorkoutGameDistanceCourse course;
};

class WorkoutGameCourseDocumentCodec
{
public:
    static constexpr int CurrentSchemaVersion = 2;
    static constexpr qsizetype MaximumDocumentBytes = 1024 * 1024;

    static bool valid(const WorkoutGameCourseDocument &document);
    static QByteArray encode(const WorkoutGameCourseDocument &document);
    static WorkoutGameCourseDocumentStatus decode(
            const QByteArray &json,
            WorkoutGameCourseDocument &document);
};

class WorkoutGameCourseDocumentStore
{
public:
    static QString sidecarPathForCourse(const QString &coursePath);
    static WorkoutGameCourseDocumentStatus saveNewArtifact(
            const QString &coursePath,
            const WorkoutGameCourseDocument &document,
            QString &error);
    static WorkoutGameCourseDocumentStatus replaceArtifact(
            const QString &coursePath,
            const WorkoutGameCourseDocument &document,
            QString &error);
    static WorkoutGameCourseDocumentStatus loadForCourse(
            const QString &coursePath,
            WorkoutGameCourseDocument &document,
            QString &error);
};

#endif
