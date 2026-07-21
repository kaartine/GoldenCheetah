/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "CompressedActivityFile.h"

#include "../../contrib/qzip/zipreader.h"

namespace CompressedActivityFile {

bool extractSingleFile(
    std::unique_ptr<QIODevice> source,
    Format format,
    QIODevice *destination)
{
    if (!source || !source->isReadable()
        || !destination || !destination->isWritable()) {
        return false;
    }

    if (format == Format::Gzip)
        return GzipReader::uncompress(source.get(), destination);

    ZipReader archive(std::move(source));
    const QList<ZipReader::FileInfo> members = archive.fileInfoList();
    ZipReader::FileInfo activity;
    int activityCount = 0;
    for (const ZipReader::FileInfo &member : members) {
        if (!member.isDir && !member.isSymLink) {
            activity = member;
            ++activityCount;
        }
    }

    return archive.status() == ZipReader::NoError
        && activityCount == 1
        && archive.extractFile(activity.filePath, destination);
}

} // namespace CompressedActivityFile
