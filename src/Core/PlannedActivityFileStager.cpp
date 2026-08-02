/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "PlannedActivityFileStager.h"

#include "RideFile.h"

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>

#include <memory>

namespace {

bool validSourceFileName(
    const QString &fileName,
    QString &format)
{
    format.clear();
    const QFileInfo info(fileName);
    if (fileName.isEmpty()
        || info.fileName() != fileName
        || fileName.contains(QLatin1Char('/'))
        || fileName.contains(QLatin1Char('\\'))) {
        return false;
    }

    format = info.suffix();
    static const QRegularExpression safeFormat(
        QStringLiteral("^[A-Za-z0-9]+$"));
    return !format.isEmpty()
        && safeFormat.match(format).hasMatch();
}

bool removeIncompleteStage(
    const QString &stagingPath,
    QString &error)
{
    const QFileInfo stagedInfo(stagingPath);
    if (!stagedInfo.exists() && !stagedInfo.isSymLink())
        return true;
    if (QFile::remove(stagingPath)) return true;

    const QString cleanupError = QObject::tr(
        "The incomplete planned activity staging file could not be removed: %1")
                                     .arg(stagingPath);
    if (error.isEmpty()) {
        error = cleanupError;
    } else {
        error.append(QStringLiteral("; "));
        error.append(cleanupError);
    }
    return false;
}

} // namespace

namespace PlannedActivityFile {

bool resolveCopyTarget(
    const QString &sourceFileName,
    const QDateTime &sourceDateTime,
    const QDate &targetDate,
    const QTime &targetTime,
    CopyTarget &target,
    QString &error)
{
    target = {};
    error.clear();

    QString format;
    if (!validSourceFileName(sourceFileName, format)) {
        error = QObject::tr(
            "The planned activity source filename is invalid");
        return false;
    }
    if (!sourceDateTime.isValid() || !targetDate.isValid()) {
        error = QObject::tr(
            "The planned activity copy date is invalid");
        return false;
    }

    const QTime effectiveTime = targetTime.isValid()
        ? targetTime
        : sourceDateTime.time();
    const QDateTime targetDateTime(targetDate, effectiveTime);
    if (!targetDateTime.isValid()) {
        error = QObject::tr(
            "The planned activity copy time is invalid");
        return false;
    }

    target.dateTime = targetDateTime;
    target.fileName = QStringLiteral("%1.%2")
        .arg(targetDateTime.toString(
                 QStringLiteral("yyyy_MM_dd_HH_mm_ss")),
             format);
    return true;
}

bool stageCopyWithAccess(
    Context *context,
    const QString &sourcePath,
    const QString &sourceFileName,
    const QDateTime &targetDateTime,
    const QString &stagingPath,
    const FileAccess &fileAccess,
    const Transform &transform,
    QString &error)
{
    error.clear();

    QString format;
    const QFileInfo sourceInfo(sourcePath);
    const QFileInfo stagingInfo(stagingPath);
    const QFileInfo stagingParentInfo(stagingInfo.absolutePath());
    if (!fileAccess.isValid()) {
        error = QObject::tr(
            "The planned activity file staging service is unavailable");
        return false;
    }
    if (!validSourceFileName(sourceFileName, format)
        || !sourceInfo.isAbsolute()
        || sourceInfo.fileName() != sourceFileName
        || !sourceInfo.exists()
        || sourceInfo.isSymLink()
        || !sourceInfo.isFile()) {
        error = QObject::tr(
            "The planned activity source is not a regular file: %1")
                    .arg(sourcePath);
        return false;
    }
    if (!targetDateTime.isValid()) {
        error = QObject::tr(
            "The planned activity target date is invalid");
        return false;
    }
    if (!stagingInfo.isAbsolute()
        || stagingInfo.exists()
        || stagingInfo.isSymLink()
        || !stagingParentInfo.exists()
        || stagingParentInfo.isSymLink()
        || !stagingParentInfo.isDir()) {
        error = QObject::tr(
            "The planned activity staging path is unavailable: %1")
                    .arg(stagingPath);
        return false;
    }

    QFile source(sourceInfo.absoluteFilePath());
    QStringList parseErrors;
    std::unique_ptr<RideFile> ride(
        fileAccess.open(context, source, parseErrors));
    if (!ride) {
        error = parseErrors.isEmpty()
            ? QObject::tr(
                  "The planned activity source could not be opened: %1")
                    .arg(sourcePath)
            : QObject::tr(
                  "The planned activity source could not be opened: %1")
                    .arg(parseErrors.join(QStringLiteral("; ")));
        return false;
    }

    ride->setStartTime(targetDateTime);
    ride->setTag(
        QStringLiteral("Year"),
        targetDateTime.toString(QStringLiteral("yyyy")));
    ride->setTag(
        QStringLiteral("Month"),
        targetDateTime.toString(QStringLiteral("MMMM")));
    ride->setTag(
        QStringLiteral("Weekday"),
        targetDateTime.toString(QStringLiteral("ddd")));
    ride->setTag(
        QStringLiteral("Original Date"),
        targetDateTime.date().toString(
            QStringLiteral("yyyy/MM/dd")));
    ride->removeTag(QStringLiteral("Linked Filename"));

    if (transform
        && !transform(ride.get(), targetDateTime, error)) {
        if (error.isEmpty()) {
            error = QObject::tr(
                "The planned activity copy transformation failed");
        }
        removeIncompleteStage(stagingPath, error);
        return false;
    }

    QFile staged(stagingInfo.absoluteFilePath());
    if (!fileAccess.write(
            context, ride.get(), staged, format)) {
        error = QObject::tr(
            "The planned activity copy could not be staged: %1")
                    .arg(sourceFileName);
        staged.close();
        removeIncompleteStage(stagingPath, error);
        return false;
    }
    staged.close();

    const QFileInfo writtenInfo(stagingPath);
    if (!writtenInfo.exists()
        || writtenInfo.isSymLink()
        || !writtenInfo.isFile()) {
        error = QObject::tr(
            "The planned activity staging writer did not create a regular file");
        removeIncompleteStage(stagingPath, error);
        return false;
    }
    return true;
}

} // namespace PlannedActivityFile
