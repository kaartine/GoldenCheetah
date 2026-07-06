/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "SplitActivitySave.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <utility>

namespace {

QString portablePathKey(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath())
        .normalized(QString::NormalizationForm_C)
        .toCaseFolded();
}

bool syncChangedDirectories(
    const QStringList &paths,
    const AtomicDirectorySyncFunction &syncDirectory,
    QString &error)
{
    QSet<QString> synced;
    for (const QString &path : paths) {
        if (path.isEmpty()) continue;
        const QString directory =
            QFileInfo(path).absolutePath();
        const QString key = atomicFilePathKey(directory);
        if (synced.contains(key)) continue;
        synced.insert(key);

        QString syncError;
        if (!syncDirectory(path, syncError)) {
            if (syncError.isEmpty()) {
                syncError = QStringLiteral(
                    "Cannot sync an activity directory");
            }
            appendAtomicFileError(error, syncError);
            return false;
        }
    }
    return true;
}

void syncRollbackDirectories(
    const QStringList &paths,
    const AtomicDirectorySyncFunction &syncDirectory,
    QString &error)
{
    QString syncError;
    if (!syncChangedDirectories(paths, syncDirectory, syncError)) {
        appendAtomicFileError(error, syncError);
    }
}

bool restorePreviousBackup(
    const QString &previousBackupPath,
    const QString &backupPath,
    const AtomicDirectorySyncFunction &syncDirectory,
    const AtomicMoveFunction &move,
    QString &error)
{
    if (previousBackupPath.isEmpty()) return true;

    QString moveError;
    if (!move(previousBackupPath, backupPath, moveError)) {
        appendAtomicFileError(
            error,
            moveError.isEmpty()
                ? QStringLiteral(
                      "cannot restore the previous activity backup")
                : moveError);
        return false;
    }
    syncRollbackDirectories(
        { previousBackupPath, backupPath }, syncDirectory, error);
    return true;
}

bool validSplitFileName(const QString &fileName)
{
    static const QRegularExpression pattern(
        QStringLiteral(
            "^[0-9]{4}_[0-9]{2}_[0-9]{2}_[0-9]{2}_[0-9]{2}_[0-9]{2}\\.json$"));
    if (!pattern.match(fileName).hasMatch()) return false;

    const QString timestamp = fileName.left(fileName.size() - 5);
    const QDateTime parsed = QDateTime::fromString(
        timestamp, QStringLiteral("yyyy_MM_dd_HH_mm_ss"));
    return parsed.isValid()
        && parsed.toString(QStringLiteral("yyyy_MM_dd_HH_mm_ss"))
            == timestamp;
}

struct PreparedSplitOutput
{
    SplitActivityOutput output;
    QString targetPath;
    QString stagingPath;
};

} // namespace

bool archiveSplitActivitySource(
    const QString &sourcePath,
    const QString &backupPath,
    QString &error,
    const AtomicDirectorySyncFunction &syncDirectory,
    bool locksHeld,
    const AtomicMoveFunction &move)
{
    error.clear();
    if (!syncDirectory || !move) {
        error = QStringLiteral(
            "Cannot archive an activity without durable file operations");
        return false;
    }

    const QFileInfo requestedSource(sourcePath);
    const QFileInfo requestedBackup(backupPath);
    const QString sourceDirectory =
        QDir(requestedSource.absolutePath()).canonicalPath();
    const QString backupDirectory =
        QDir(requestedBackup.absolutePath()).canonicalPath();
    if (requestedSource.fileName().isEmpty()
        || requestedBackup.fileName().isEmpty()
        || sourceDirectory.isEmpty()
        || backupDirectory.isEmpty()) {
        error = QStringLiteral("Invalid activity archive paths");
        return false;
    }

    const QString source =
        QDir(sourceDirectory).filePath(requestedSource.fileName());
    const QString backup =
        QDir(backupDirectory).filePath(requestedBackup.fileName());
    if (portablePathKey(source) == portablePathKey(backup)) {
        error = QStringLiteral("Invalid activity archive paths");
        return false;
    }

    AtomicFileLockSet locks;
    if (!locksHeld && !locks.lock({ source, backup }, error)) {
        return false;
    }

    const QFileInfo sourceInfo(source);
    if (!sourceInfo.exists() || !sourceInfo.isFile()
        || sourceInfo.isSymLink()) {
        error = QStringLiteral(
            "The source activity is unavailable or unsafe");
        return false;
    }

    const QFileInfo backupDirectoryInfo(backupDirectory);
    if (!backupDirectoryInfo.exists()
        || !backupDirectoryInfo.isDir()
        || backupDirectoryInfo.isSymLink()) {
        error = QStringLiteral(
            "The activity backup directory is unavailable or unsafe");
        return false;
    }

    const QFileInfo priorBackupInfo(backup);
    const bool hadPriorBackup =
        priorBackupInfo.exists() || priorBackupInfo.isSymLink();
    if (hadPriorBackup
        && (priorBackupInfo.isSymLink()
            || !priorBackupInfo.isFile())) {
        error = QStringLiteral(
            "The existing activity backup is unsafe");
        return false;
    }

    QString previousBackupPath;
    if (hadPriorBackup) {
        previousBackupPath =
            backup + QStringLiteral(".rollback-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);

        QString preserveError;
        if (!move(backup, previousBackupPath, preserveError)) {
            error = QStringLiteral(
                "Cannot preserve the previous activity backup");
            if (!preserveError.isEmpty()) {
                appendAtomicFileError(error, preserveError);
            }
            return false;
        }

        if (!syncChangedDirectories(
                { backup, previousBackupPath },
                syncDirectory, error)) {
            restorePreviousBackup(
                previousBackupPath, backup,
                syncDirectory, move, error);
            return false;
        }
    }

    QString archiveError;
    if (!move(source, backup, archiveError)) {
        error = QStringLiteral(
            "Cannot archive the original activity");
        if (!archiveError.isEmpty()) {
            appendAtomicFileError(error, archiveError);
        }
        restorePreviousBackup(
            previousBackupPath, backup,
            syncDirectory, move, error);
        return false;
    }

    QString syncError;
    if (!syncChangedDirectories(
            { source, backup }, syncDirectory, syncError)) {
        error = syncError;

        QString rollbackError;
        const bool sourceRestored =
            move(backup, source, rollbackError);
        if (!sourceRestored) {
            appendAtomicFileError(
                error,
                rollbackError.isEmpty()
                    ? QStringLiteral(
                          "cannot restore the original activity")
                    : rollbackError);
            appendAtomicFileError(
                error,
                QStringLiteral(
                    "the original remains archived; split files were kept"));
            syncRollbackDirectories(
                { source, backup, previousBackupPath },
                syncDirectory, error);
            return true;
        }

        restorePreviousBackup(
            previousBackupPath, backup,
            syncDirectory, move, error);
        syncRollbackDirectories(
            { source, backup, previousBackupPath },
            syncDirectory, error);
        return false;
    }

    if (!previousBackupPath.isEmpty()) {
        if (!QFile::remove(previousBackupPath)) {
            appendAtomicFileError(
                error,
                QStringLiteral(
                    "cannot remove the previous activity backup"));
        } else {
            QString cleanupError;
            if (!syncChangedDirectories(
                    { previousBackupPath },
                    syncDirectory, cleanupError)) {
                appendAtomicFileError(error, cleanupError);
            }
        }
    }
    return true;
}

bool saveSplitActivityFiles(
    const QDir &activitiesDirectory,
    const QString &sourcePath,
    const QString &backupPath,
    const QList<SplitActivityOutput> &outputs,
    bool keepOriginal,
    QStringList &publishedFileNames,
    QString &error,
    const AtomicPublishFunction &publish,
    const SplitActivityArchiveFunction &archive)
{
    error.clear();
    publishedFileNames.clear();

    const QString activitiesPath =
        activitiesDirectory.canonicalPath();
    if (activitiesPath.isEmpty()) {
        error = QStringLiteral(
            "The activity directory is unavailable");
        return false;
    }

    const QFileInfo requestedSourceInfo(sourcePath);
    const QString sourceDirectory =
        QDir(requestedSourceInfo.absolutePath()).canonicalPath();
    const QString source = QDir(activitiesPath).filePath(
        requestedSourceInfo.fileName());
    const QFileInfo sourceInfo(source);
    if (requestedSourceInfo.fileName().isEmpty()
        || sourceDirectory != activitiesPath
        || !sourceInfo.exists() || !sourceInfo.isFile()
        || sourceInfo.isSymLink()) {
        error = QStringLiteral(
            "The source activity is unavailable or unsafe");
        return false;
    }

    if (outputs.isEmpty()) {
        error = QStringLiteral(
            "No split activity files to save");
        return false;
    }

    QString backup;
    if (!keepOriginal) {
        const QFileInfo backupInfo(backupPath);
        const QString backupDirectory =
            QDir(backupInfo.absolutePath()).canonicalPath();
        if (backupInfo.fileName().isEmpty()
            || backupDirectory.isEmpty()) {
            error = QStringLiteral(
                "The activity backup path is unavailable");
            return false;
        }
        backup = QDir(backupDirectory).filePath(
            backupInfo.fileName());
        if (portablePathKey(source)
            == portablePathKey(backup)) {
            error = QStringLiteral(
                "The activity source and backup paths overlap");
            return false;
        }
    }

    QList<PreparedSplitOutput> prepared;
    QSet<QString> targetKeys;
    for (const SplitActivityOutput &output : outputs) {
        if (!output.stage
            || !validSplitFileName(output.fileName)) {
            error = QStringLiteral(
                "Invalid split activity file name");
            return false;
        }

        PreparedSplitOutput item;
        item.output = output;
        item.targetPath =
            QDir(activitiesPath).filePath(output.fileName);
        const QString targetKey =
            portablePathKey(item.targetPath);
        if (targetKeys.contains(targetKey)) {
            error = QStringLiteral(
                "Duplicate split activity file name");
            return false;
        }
        targetKeys.insert(targetKey);

        if (targetKey == portablePathKey(source)
            || (!keepOriginal
                && targetKey == portablePathKey(backup))) {
            error = QStringLiteral(
                "A split activity target overlaps a reserved path");
            return false;
        }

        const QFileInfo targetInfo(item.targetPath);
        if (targetInfo.exists() || targetInfo.isSymLink()) {
            error = QStringLiteral(
                "A split activity target already exists");
            return false;
        }
        prepared.append(item);
    }

    AtomicFileLockSet sourceLocks;
    AtomicFileSnapshot sourceSnapshot;
    if (!keepOriginal) {
        if (!sourceLocks.lock({ source, backup }, error)
            || !captureAtomicFileSnapshot(
                source, sourceSnapshot, error)) {
            return false;
        }
    }

    QList<StagedFilePublication> publications;
    for (PreparedSplitOutput &item : prepared) {
        item.stagingPath = QDir(activitiesPath).filePath(
            QStringLiteral(".%1.%2.split-stage")
                .arg(
                    item.output.fileName,
                    QUuid::createUuid().toString(
                        QUuid::WithoutBraces)));
        publications.append(
            StagedFilePublication(
                item.stagingPath, item.targetPath));

        QString stageError;
        if (!item.output.stage(
                item.stagingPath, stageError)) {
            error = stageError.isEmpty()
                ? QStringLiteral(
                    "Cannot stage a split activity file")
                : stageError;
            cleanupStagedFiles(publications, error);
            return false;
        }
    }

    AtomicFinalizeFunction finalize;
    if (!keepOriginal) {
        const SplitActivityArchiveFunction archiveSource =
            archive ? archive
                    : SplitActivityArchiveFunction(
                          [](const QString &sourceFile,
                             const QString &backupFile,
                             QString &archiveError) {
                              return archiveSplitActivitySource(
                                  sourceFile, backupFile,
                                  archiveError,
                                  syncParentDirectory, true);
                          });
        finalize =
            [source, sourceSnapshot, backup, archiveSource](
                QString &finalizeError) {
                if (!atomicFileMatchesSnapshot(
                        source, sourceSnapshot,
                        finalizeError)) {
                    return false;
                }
                return archiveSource(
                    source, backup, finalizeError);
            };
    }

    if (!publishStagedFileSet(
            publications, error, publish, finalize)) {
        return false;
    }

    for (const PreparedSplitOutput &item : std::as_const(prepared)) {
        publishedFileNames.append(item.output.fileName);
    }
    return true;
}
