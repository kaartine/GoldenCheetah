/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_LibraryImportFileStager_h
#define _GC_LibraryImportFileStager_h

#include <QHash>
#include <QString>
#include <QStringList>

enum class LibraryImportStageStatus
{
    ready,
    copied,
    targetConflict,
    ioError
};

struct LibraryImportStageResult
{
    LibraryImportStageStatus status = LibraryImportStageStatus::ioError;
    QString error;

    bool succeeded() const
    {
        return status == LibraryImportStageStatus::ready
            || status == LibraryImportStageStatus::copied;
    }

    bool created() const
    {
        return status == LibraryImportStageStatus::copied;
    }
};

class LibraryImportFileStager
{
public:
    LibraryImportStageResult stage(const QString &sourcePath,
                                   const QString &targetPath);
    QStringList rollback();

private:
    QHash<QString, QString> sourcesByTarget;
    QStringList createdTargets;
};

#endif // _GC_LibraryImportFileStager_h
