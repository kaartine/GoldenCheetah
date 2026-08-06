/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "ErgFile.h"
#include "GpxParser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <memory>

ErgFile *ErgFile::fromContentBytes(
    const QByteArray &contents,
    const QString &sourceFileName,
    ErgFileFormat mode,
    Context *context,
    QString &error,
    const std::function<bool()> &cancelled,
    QDate when)
{
    error.clear();
    const QString fileName = QFileInfo(sourceFileName).fileName();
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (contents.isEmpty() || fileName.isEmpty()
        || !isWorkout(fileName) || suffix.isEmpty()) {
        error = QObject::tr("The workout content is invalid.");
        return nullptr;
    }
    if (suffix == QStringLiteral("gpx")) {
        return fromGpxContentBytes(
            contents, sourceFileName, mode, context,
            error, cancelled, when);
    }
    if (!context) {
        error = QObject::tr("The workout context is unavailable.");
        return nullptr;
    }

    QTemporaryDir directory;
    if (!directory.isValid()) {
        error = QObject::tr(
            "A private workout parsing directory could not be created.");
        return nullptr;
    }
    const QString path = QDir(directory.path()).filePath(
        QStringLiteral("workout.%1").arg(suffix));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || file.write(contents) != contents.size()
        || !file.flush()) {
        error = QObject::tr("The workout content could not be staged.");
        return nullptr;
    }
    file.close();

    std::unique_ptr<ErgFile> workout(
        new ErgFile(path, mode, context, when));
    if (!workout->isValid()) {
        error = QObject::tr("The workout content is not valid.");
        return nullptr;
    }
    workout->filename(sourceFileName);
    return workout.release();
}

ErgFile *ErgFile::fromGpxContentBytes(
    const QByteArray &contents,
    const QString &sourceFileName,
    ErgFileFormat mode,
    Context *context,
    QString &error,
    const std::function<bool()> &cancelled,
    QDate when)
{
    return fromGpxContentBytes(
        contents, sourceFileName, mode, context,
        GpxParser::captureOptions(), error, cancelled, when);
}

ErgFile *ErgFile::fromGpxContentBytes(
    const QByteArray &contents,
    const QString &sourceFileName,
    ErgFileFormat mode,
    Context *context,
    const GpxParserOptions &options,
    QString &error,
    const std::function<bool()> &cancelled,
    QDate when)
{
    error.clear();
    const QString fileName = QFileInfo(sourceFileName).fileName();
    if (contents.isEmpty() || fileName.isEmpty()
        || QFileInfo(fileName).suffix().compare(
            QStringLiteral("gpx"), Qt::CaseInsensitive) != 0) {
        error = QObject::tr("The GPX workout content is invalid.");
        return nullptr;
    }

    QTemporaryDir directory;
    if (!directory.isValid()) {
        error = QObject::tr(
            "A private workout parsing directory could not be created.");
        return nullptr;
    }
    const QString path = QDir(directory.path()).filePath(
        QStringLiteral("workout.gpx"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        error = QObject::tr("The workout content could not be staged.");
        return nullptr;
    }
    constexpr qsizetype StagingChunkSize = 256 * 1024;
    for (qsizetype offset = 0; offset < contents.size();
         offset += StagingChunkSize) {
        bool cancellationRequested = false;
        if (cancelled) {
            try {
                cancellationRequested = cancelled();
            } catch (...) {
                cancellationRequested = true;
            }
        }
        if (cancellationRequested) {
            error = QObject::tr("GPX parsing was cancelled.");
            return nullptr;
        }
        const qsizetype size = qMin(
            StagingChunkSize, contents.size() - offset);
        if (file.write(contents.constData() + offset, size) != size) {
            error = QObject::tr(
                "The workout content could not be staged.");
            return nullptr;
        }
    }
    if (!file.flush()) {
        error = QObject::tr("The workout content could not be staged.");
        return nullptr;
    }
    file.close();

#ifdef GC_ERG_FILE_GPX_TEST_HOOKS
    extern void ergFileByteBackingPathTestHook(const QString &path);
    ergFileByteBackingPathTestHook(path);
#endif

    std::unique_ptr<ErgFile> workout(new ErgFile(context, when));
    workout->filename(path);
    workout->mode(mode);
    if (!workout->parseGpxFile(options, cancelled, error)) return nullptr;
    workout->filename(sourceFileName);
    return workout.release();
}
