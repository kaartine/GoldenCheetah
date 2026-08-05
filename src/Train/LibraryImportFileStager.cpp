/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "LibraryImportFileStager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>

namespace {

bool compareFileContents(const QString &leftPath,
                         const QString &rightPath,
                         bool &equal,
                         QString &error)
{
    equal = false;
    QFile left(leftPath);
    QFile right(rightPath);
    if (!left.open(QIODevice::ReadOnly)) {
        error = left.errorString();
        return false;
    }
    if (!right.open(QIODevice::ReadOnly)) {
        error = right.errorString();
        return false;
    }
    if (left.size() != right.size()) {
        return true;
    }

    constexpr qint64 chunkSize = 1024 * 1024;
    while (!left.atEnd() || !right.atEnd()) {
        const QByteArray leftChunk = left.read(chunkSize);
        const QByteArray rightChunk = right.read(chunkSize);
        if (left.error() != QFileDevice::NoError) {
            error = left.errorString();
            return false;
        }
        if (right.error() != QFileDevice::NoError) {
            error = right.errorString();
            return false;
        }
        if (leftChunk != rightChunk) {
            return true;
        }
    }

    equal = true;
    return true;
}

LibraryImportStageResult failure(LibraryImportStageStatus status,
                                 const QString &error)
{
    LibraryImportStageResult result;
    result.status = status;
    result.error = error;
    return result;
}

bool copyContents(QFile &source, QIODevice &target, QString &error)
{
    constexpr qint64 chunkSize = 1024 * 1024;
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(chunkSize);
        if (source.error() != QFileDevice::NoError) {
            error = source.errorString();
            return false;
        }
        if (target.write(chunk) != chunk.size()) {
            error = target.errorString();
            return false;
        }
    }
    return true;
}

bool copyAtomically(const QString &sourcePath,
                    const QString &targetPath,
                    QFileDevice::Permissions permissions,
                    QString &error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        error = source.errorString();
        return false;
    }

    QSaveFile target(targetPath);
    target.setDirectWriteFallback(false);
    if (!target.open(QIODevice::WriteOnly)) {
        error = target.errorString();
        return false;
    }
    if (!copyContents(source, target, error)) {
        target.cancelWriting();
        return false;
    }
    if (!target.setPermissions(permissions)) {
        error = target.errorString();
        target.cancelWriting();
        return false;
    }
    if (!target.commit()) {
        error = target.errorString();
        return false;
    }
    return true;
}

bool createBackup(const QString &targetPath,
                  QString &backupPath,
                  QString &error)
{
    QFile source(targetPath);
    if (!source.open(QIODevice::ReadOnly)) {
        error = source.errorString();
        return false;
    }

    const QString pattern = QDir(QFileInfo(targetPath).absolutePath())
                                .filePath(QStringLiteral(
                                    ".gc-library-import-backup-XXXXXX"));
    QTemporaryFile backup(pattern);
    if (!backup.open()) {
        error = backup.errorString();
        return false;
    }
    if (!copyContents(source, backup, error) || !backup.flush()) {
        if (error.isEmpty()) error = backup.errorString();
        return false;
    }
    if (!backup.setPermissions(QFileInfo(targetPath).permissions())) {
        error = backup.errorString();
        return false;
    }

    backupPath = backup.fileName();
    backup.setAutoRemove(false);
    backup.close();
    return true;
}

} // namespace

LibraryImportStageResult LibraryImportFileStager::stage(
    const QString &sourcePath,
    const QString &targetPath,
    LibraryImportStageMode mode)
{
    const QFileInfo sourceInfo(sourcePath);
    const QString source = sourceInfo.absoluteFilePath();
    const QString target = QFileInfo(targetPath).absoluteFilePath();

    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        return failure(
            LibraryImportStageStatus::ioError,
            QStringLiteral("The import source is not a readable file"));
    }

    const auto prepared = sourcesByTarget.constFind(target);
    if (prepared != sourcesByTarget.cend()) {
        if (prepared.value() == source) {
            return {LibraryImportStageStatus::ready, {}};
        }
        return failure(
            LibraryImportStageStatus::targetConflict,
            QStringLiteral("The target is already prepared from another source"));
    }

    if (source == target) {
        sourcesByTarget.insert(target, source);
        return {LibraryImportStageStatus::ready, {}};
    }

    const QFileInfo targetInfo(target);
    if (targetInfo.isSymLink()) {
        return failure(
            LibraryImportStageStatus::targetConflict,
            QStringLiteral("The import target is a symbolic link"));
    }
    if (targetInfo.exists()) {
        if (!targetInfo.isFile()) {
            return failure(
                LibraryImportStageStatus::targetConflict,
                QStringLiteral("The import target is not a regular file"));
        }

        bool equal = false;
        QString compareError;
        if (!compareFileContents(source, target, equal, compareError)) {
            return failure(LibraryImportStageStatus::ioError, compareError);
        }
        if (!equal && mode == LibraryImportStageMode::preserveExisting) {
            return failure(
                LibraryImportStageStatus::targetConflict,
                QStringLiteral(
                    "The import target already exists with different contents"));
        }

        if (!equal) {
            QString backupPath;
            if (!createBackup(target, backupPath, compareError)) {
                return failure(LibraryImportStageStatus::ioError,
                               compareError);
            }
            if (!copyAtomically(source,
                                target,
                                sourceInfo.permissions(),
                                compareError)) {
                if (!QFile::remove(backupPath)) {
                    compareError += QStringLiteral(
                        " The preserved target remains at %1.")
                                        .arg(backupPath);
                }
                return failure(LibraryImportStageStatus::ioError,
                               compareError);
            }

            sourcesByTarget.insert(target, source);
            replacedTargets.append(
                {target, backupPath, targetInfo.permissions()});
            return {LibraryImportStageStatus::replaced, {}};
        }

        sourcesByTarget.insert(target, source);
        return {LibraryImportStageStatus::ready, {}};
    }

    QFile sourceFile(source);
    if (!sourceFile.copy(target)) {
        return failure(
            LibraryImportStageStatus::ioError,
            sourceFile.errorString());
    }

    sourcesByTarget.insert(target, source);
    createdTargets.append(target);
    return {LibraryImportStageStatus::copied, {}};
}

QStringList LibraryImportFileStager::rollback()
{
    QStringList failures;

    for (int index = replacedTargets.count() - 1; index >= 0; --index) {
        const ReplacedTarget &replacement = replacedTargets.at(index);
        QString error;
        if (!copyAtomically(replacement.backup,
                            replacement.target,
                            replacement.permissions,
                            error)) {
            failures.append(replacement.target);
            failures.append(replacement.backup);
            continue;
        }
        if (!QFile::remove(replacement.backup)) {
            failures.append(replacement.backup);
        }
    }
    for (int index = createdTargets.count() - 1; index >= 0; --index) {
        const QString &target = createdTargets.at(index);
        if (QFileInfo::exists(target) && !QFile::remove(target)) {
            failures.append(target);
        }
    }
    replacedTargets.clear();
    createdTargets.clear();
    sourcesByTarget.clear();
    return failures;
}

QStringList LibraryImportFileStager::finalize()
{
    QStringList failures;
    for (const ReplacedTarget &replacement : replacedTargets) {
        if (QFileInfo::exists(replacement.backup)
            && !QFile::remove(replacement.backup)) {
            failures.append(replacement.backup);
        }
    }
    replacedTargets.clear();
    createdTargets.clear();
    sourcesByTarget.clear();
    return failures;
}
