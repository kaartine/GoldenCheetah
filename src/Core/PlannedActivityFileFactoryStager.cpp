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

namespace PlannedActivityFile {

bool stageCopy(
    Context *context,
    const QString &sourcePath,
    const QString &sourceFileName,
    const QDateTime &targetDateTime,
    const QString &stagingPath,
    const Transform &transform,
    QString &error)
{
    const FileAccess fileAccess {
        [](Context *openContext, QFile &source,
           QStringList &errors) {
            return RideFileFactory::instance().openRideFile(
                openContext, source, errors);
        },
        [](Context *writeContext, const RideFile *ride,
           QFile &target, const QString &format) {
            return RideFileFactory::instance().writeRideFile(
                writeContext, ride, target, format);
        }
    };
    return stageCopyWithAccess(
        context, sourcePath, sourceFileName,
        targetDateTime, stagingPath, fileAccess,
        transform, error);
}

} // namespace PlannedActivityFile
