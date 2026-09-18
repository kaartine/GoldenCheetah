/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_WorkoutDeletionService_h
#define _GC_WorkoutDeletionService_h

#include <QString>
#include <QStringList>

#include <functional>

class TrainDB;

struct WorkoutDeletionResult
{
    bool succeeded = false;
    QString errorMessage;
    QString warningMessage;
    QString failedPath;
    QStringList deletedWorkouts;
    QStringList rollbackFailures;
    QStringList cleanupFailures;
    QStringList residualStagedFiles;
};

struct WorkoutDeletionOperations
{
    std::function<bool(const QString &)> pathExists;
    std::function<bool(const QString &)> isRegularFile;
    std::function<bool(const QString &)> isSymbolicLink;
    std::function<bool(const QString &, const QString &, QString &)> renameFile;
    std::function<bool(const QString &, QString &)> removeFile;

    std::function<bool()> beginDatabaseTransaction;
    std::function<bool(const QString &)> deleteDatabaseWorkout;
    std::function<bool()> commitDatabaseTransaction;
    std::function<void()> rollbackDatabaseTransaction;
};

QString workoutDeletionSidecarPath(const QString &workoutPath);
WorkoutDeletionOperations defaultWorkoutDeletionFileOperations();
WorkoutDeletionOperations workoutDeletionOperations(TrainDB &database);

WorkoutDeletionResult deleteWorkoutsAtomically(
        const QStringList &workoutPaths,
        const WorkoutDeletionOperations &operations);
WorkoutDeletionResult deleteWorkoutsAtomically(
        TrainDB &database,
        const QStringList &workoutPaths);

#endif // _GC_WorkoutDeletionService_h
