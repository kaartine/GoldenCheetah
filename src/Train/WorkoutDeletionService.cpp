/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "WorkoutDeletionService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSet>
#include <QUuid>

namespace {

struct DeletionFile
{
    QString originalPath;
    QString stagedPath;
};

void appendUnique(QStringList &values, const QString &value)
{
    if (!values.contains(value)) values.append(value);
}

bool completeOperations(const WorkoutDeletionOperations &operations)
{
    return operations.pathExists
        && operations.isRegularFile
        && operations.isSymbolicLink
        && operations.renameFile
        && operations.removeFile
        && operations.beginDatabaseTransaction
        && operations.deleteDatabaseWorkout
        && operations.commitDatabaseTransaction
        && operations.rollbackDatabaseTransaction;
}

QString uniqueStagingPath(const QString &sourcePath,
                          const WorkoutDeletionOperations &operations)
{
    const QFileInfo source(sourcePath);
    const QDir directory(source.absolutePath());
    for (int attempt = 0; attempt < 32; ++attempt) {
        const QString candidate = directory.filePath(
                QStringLiteral(".gc-delete-%1-%2")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces),
                         source.fileName()));
        if (!operations.pathExists(candidate)
                && !operations.isSymbolicLink(candidate)) {
            return candidate;
        }
    }
    return {};
}

void appendRecoveryDetails(WorkoutDeletionResult &result)
{
    if (result.rollbackFailures.isEmpty()) return;

    result.errorMessage += QObject::tr(
            " Some files could not be restored to their original locations: %1.")
                                   .arg(result.rollbackFailures.join(
                                           QStringLiteral(", ")));
    if (!result.residualStagedFiles.isEmpty()) {
        result.errorMessage += QObject::tr(
                " Recovery copies remain at: %1.")
                                       .arg(result.residualStagedFiles.join(
                                               QStringLiteral(", ")));
    }
}

void rollbackFiles(QList<DeletionFile> &staged,
                   const WorkoutDeletionOperations &operations,
                   WorkoutDeletionResult &result)
{
    for (int index = staged.size() - 1; index >= 0; --index) {
        const DeletionFile &file = staged.at(index);
        QString error;
        if (!operations.renameFile(
                    file.stagedPath, file.originalPath, error)) {
            appendUnique(result.rollbackFailures, file.originalPath);
            appendUnique(result.residualStagedFiles, file.stagedPath);
        }
    }
    appendRecoveryDetails(result);
}

WorkoutDeletionResult failBeforeDatabase(
        const QString &message,
        const QString &path,
        QList<DeletionFile> &staged,
        const WorkoutDeletionOperations &operations)
{
    WorkoutDeletionResult result;
    result.errorMessage = message;
    result.failedPath = path;
    rollbackFiles(staged, operations, result);
    return result;
}

} // namespace

QString workoutDeletionSidecarPath(const QString &workoutPath)
{
    const QFileInfo workout(workoutPath);
    return workout.dir().filePath(
            workout.completeBaseName() + QStringLiteral(".gcmtb.json"));
}

WorkoutDeletionOperations defaultWorkoutDeletionFileOperations()
{
    WorkoutDeletionOperations operations;
    operations.pathExists = [](const QString &path) {
        return QFileInfo::exists(path);
    };
    operations.isRegularFile = [](const QString &path) {
        return QFileInfo(path).isFile();
    };
    operations.isSymbolicLink = [](const QString &path) {
        return QFileInfo(path).isSymLink();
    };
    operations.renameFile = [](const QString &from,
                               const QString &to,
                               QString &error) {
        QFile file(from);
        if (file.rename(to)) return true;
        error = file.errorString();
        return false;
    };
    operations.removeFile = [](const QString &path, QString &error) {
        QFile file(path);
        if (file.remove()) return true;
        error = file.errorString();
        return false;
    };
    return operations;
}

WorkoutDeletionResult deleteWorkoutsAtomically(
        const QStringList &workoutPaths,
        const WorkoutDeletionOperations &operations)
{
    WorkoutDeletionResult result;
    if (!completeOperations(operations)) {
        result.errorMessage = QObject::tr(
                "Workout deletion is not configured correctly.");
        return result;
    }

    QStringList workouts;
    QSet<QString> seenWorkouts;
    for (const QString &path : workoutPaths) {
        const QString absolutePath = QFileInfo(path).absoluteFilePath();
        if (seenWorkouts.contains(absolutePath)) continue;
        seenWorkouts.insert(absolutePath);
        // The database key must remain byte-for-byte compatible with the row
        // selected by its model, even if an older database contains a relative
        // path. File operations use an absolute path below.
        workouts.append(path);
    }
    if (workouts.isEmpty()) {
        result.errorMessage = QObject::tr("No workouts were selected for deletion.");
        return result;
    }

    QList<DeletionFile> files;
    QSet<QString> seenFiles;
    for (const QString &workoutKey : workouts) {
        const QString workout = QFileInfo(workoutKey).absoluteFilePath();
        if (operations.isSymbolicLink(workout)) {
            result.failedPath = workoutKey;
            result.errorMessage = QObject::tr(
                    "%1 is a symbolic link. Remove it from the library instead of deleting its target.")
                                          .arg(workout);
            return result;
        }
        if (!operations.pathExists(workout)
                || !operations.isRegularFile(workout)) {
            result.failedPath = workout;
            result.errorMessage = QObject::tr(
                    "%1 is missing or is not a regular workout file. No workouts were deleted.")
                                          .arg(workout);
            return result;
        }
        if (!seenFiles.contains(workout)) {
            seenFiles.insert(workout);
            files.append({workout, {}});
        }

        const QString sidecar = workoutDeletionSidecarPath(workout);
        if (operations.isSymbolicLink(sidecar)) {
            result.failedPath = sidecar;
            result.errorMessage = QObject::tr(
                    "%1 is a symbolic link. No workouts were deleted.")
                                          .arg(sidecar);
            return result;
        }
        if (!operations.pathExists(sidecar)) continue;
        if (!operations.isRegularFile(sidecar)) {
            result.failedPath = sidecar;
            result.errorMessage = QObject::tr(
                    "%1 exists but is not a regular MTB metadata file. No workouts were deleted.")
                                          .arg(sidecar);
            return result;
        }
        if (!seenFiles.contains(sidecar)) {
            seenFiles.insert(sidecar);
            files.append({sidecar, {}});
        }
    }

    QList<DeletionFile> staged;
    for (DeletionFile file : files) {
        file.stagedPath = uniqueStagingPath(file.originalPath, operations);
        if (file.stagedPath.isEmpty()) {
            return failBeforeDatabase(
                    QObject::tr(
                            "Could not reserve a staging name for %1. No database rows were removed.")
                            .arg(file.originalPath),
                    file.originalPath,
                    staged,
                    operations);
        }

        QString error;
        if (!operations.renameFile(
                    file.originalPath, file.stagedPath, error)) {
            return failBeforeDatabase(
                    QObject::tr(
                            "Could not stage %1 for deletion: %2. No database rows were removed.")
                            .arg(file.originalPath,
                                 error.isEmpty()
                                     ? QObject::tr("unknown file error")
                                     : error),
                    file.originalPath,
                    staged,
                    operations);
        }
        staged.append(file);
    }

    if (!operations.beginDatabaseTransaction()) {
        return failBeforeDatabase(
                QObject::tr(
                        "Could not start the workout database transaction. No workouts were deleted."),
                {},
                staged,
                operations);
    }

    for (const QString &workout : workouts) {
        if (operations.deleteDatabaseWorkout(workout)) continue;

        result.errorMessage = QObject::tr(
                "Could not remove %1 from the workout database. No workouts were deleted.")
                                      .arg(workout);
        result.failedPath = workout;
        operations.rollbackDatabaseTransaction();
        rollbackFiles(staged, operations, result);
        return result;
    }

    if (!operations.commitDatabaseTransaction()) {
        result.errorMessage = QObject::tr(
                "Could not commit the workout database transaction. No workouts were deleted.");
        operations.rollbackDatabaseTransaction();
        rollbackFiles(staged, operations, result);
        return result;
    }

    result.succeeded = true;
    result.deletedWorkouts = workouts;
    for (const DeletionFile &file : staged) {
        QString error;
        if (!operations.removeFile(file.stagedPath, error)) {
            appendUnique(result.cleanupFailures, file.originalPath);
            appendUnique(result.residualStagedFiles, file.stagedPath);
        }
    }
    if (!result.cleanupFailures.isEmpty()) {
        result.warningMessage = QObject::tr(
                "The workouts were removed from the library, but cleanup of staged files failed for: %1. Residual files remain at: %2.")
                                        .arg(result.cleanupFailures.join(
                                                 QStringLiteral(", ")),
                                             result.residualStagedFiles.join(
                                                 QStringLiteral(", ")));
    }
    return result;
}
