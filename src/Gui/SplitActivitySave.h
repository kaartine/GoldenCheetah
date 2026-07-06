/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_SplitActivitySave_h
#define _GC_SplitActivitySave_h

#include "AtomicFileWriter.h"

#include <QDir>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

struct SplitActivityOutput
{
    QString fileName;
    std::function<bool(const QString &stagingPath, QString &error)> stage;
};

using SplitActivityArchiveFunction = std::function<bool(
    const QString &sourcePath, const QString &backupPath, QString &error)>;

bool archiveSplitActivitySource(
    const QString &sourcePath,
    const QString &backupPath,
    QString &error,
    const AtomicDirectorySyncFunction &syncDirectory = syncParentDirectory,
    bool locksHeld = false,
    const AtomicMoveFunction &move = moveAtomicFile);

bool saveSplitActivityFiles(
    const QDir &activitiesDirectory,
    const QString &sourcePath,
    const QString &backupPath,
    const QList<SplitActivityOutput> &outputs,
    bool keepOriginal,
    QStringList &publishedFileNames,
    QString &error,
    const AtomicPublishFunction &publish = publishAtomicNew,
    const SplitActivityArchiveFunction &archive =
        SplitActivityArchiveFunction());

#endif // _GC_SplitActivitySave_h
