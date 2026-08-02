/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_PlannedActivityFileStager_h
#define _GC_PlannedActivityFileStager_h

#include <QDate>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QTime>

#include <functional>

class Context;
class QFile;
class RideFile;

namespace PlannedActivityFile {

struct CopyTarget
{
    QString fileName;
    QDateTime dateTime;
};

using Transform = std::function<bool(
    RideFile *ride,
    const QDateTime &targetDateTime,
    QString &error)>;

struct FileAccess
{
    std::function<RideFile *(
        Context *context,
        QFile &source,
        QStringList &errors)> open;
    std::function<bool(
        Context *context,
        const RideFile *ride,
        QFile &target,
        const QString &format)> write;

    bool isValid() const
    {
        return static_cast<bool>(open)
            && static_cast<bool>(write);
    }
};

bool resolveCopyTarget(
    const QString &sourceFileName,
    const QDateTime &sourceDateTime,
    const QDate &targetDate,
    const QTime &targetTime,
    CopyTarget &target,
    QString &error);

bool stageCopy(
    Context *context,
    const QString &sourcePath,
    const QString &sourceFileName,
    const QDateTime &targetDateTime,
    const QString &stagingPath,
    const Transform &transform,
    QString &error);

bool stageCopyWithAccess(
    Context *context,
    const QString &sourcePath,
    const QString &sourceFileName,
    const QDateTime &targetDateTime,
    const QString &stagingPath,
    const FileAccess &fileAccess,
    const Transform &transform,
    QString &error);

} // namespace PlannedActivityFile

#endif // _GC_PlannedActivityFileStager_h
