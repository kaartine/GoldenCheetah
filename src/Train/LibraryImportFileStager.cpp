/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "LibraryImportFileStager.h"

#include <QFile>
#include <QFileInfo>

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

} // namespace

LibraryImportStageResult LibraryImportFileStager::stage(
    const QString &sourcePath,
    const QString &targetPath)
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
        if (!equal) {
            return failure(
                LibraryImportStageStatus::targetConflict,
                QStringLiteral(
                    "The import target already exists with different contents"));
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
    for (int index = createdTargets.count() - 1; index >= 0; --index) {
        const QString &target = createdTargets.at(index);
        if (QFileInfo::exists(target) && !QFile::remove(target)) {
            failures.append(target);
        }
    }
    createdTargets.clear();
    sourcesByTarget.clear();
    return failures;
}
