/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_RIDEFILETEMPORARYWORKSPACE_H
#define GC_RIDEFILETEMPORARYWORKSPACE_H

#include <QFile>
#include <QString>

#include <memory>

class RideFileTemporaryFile
{
public:
    virtual ~RideFileTemporaryFile() = default;
    virtual QFile &file() = 0;
    virtual QFile *fileForReading() = 0;
};

class RideFileTemporaryWorkspace
{
public:
    virtual ~RideFileTemporaryWorkspace() = default;

    virtual std::unique_ptr<RideFileTemporaryFile> createForSuffix(
        const QString &suffix) const = 0;
};

std::shared_ptr<const RideFileTemporaryWorkspace>
rideFileTemporaryWorkspaceForRoot(const QString &rootPath);

struct RideFileOpenInputs
{
    std::shared_ptr<const RideFileTemporaryWorkspace>
        compressedTemporaryFiles;
};

#endif // GC_RIDEFILETEMPORARYWORKSPACE_H
