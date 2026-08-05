/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_TrainSidebarRuntime_h
#define _GC_TrainSidebarRuntime_h

#include <QFile>
#include <QIODevice>
#include <QStringList>

namespace TrainSidebarRuntime
{

enum class AuxiliaryHeader {
    Rr,
    CoreTemperature
};

inline QIODevice *auxiliaryHeaderDevice(
        AuxiliaryHeader header,
        QFile *rrFile,
        QFile *coreTemperatureFile)
{
    switch (header) {
    case AuxiliaryHeader::Rr:
        return rrFile;
    case AuxiliaryHeader::CoreTemperature:
        return coreTemperatureFile;
    }
    return nullptr;
}

inline QStringList recordingArtifactPaths(const QString &recordingPath)
{
    QString basePath = recordingPath;
    if (basePath.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive)) {
        basePath.chop(4);
    }
    return {
        recordingPath,
        basePath + QStringLiteral(".rr"),
        basePath + QStringLiteral(".pos.csv"),
        basePath + QStringLiteral(".vo2"),
        basePath + QStringLiteral(".tcr")
    };
}

inline bool discardRecordingArtifacts(const QString &recordingPath)
{
    bool removed = true;
    for (const QString &path : recordingArtifactPaths(recordingPath)) {
        QFile artifact(path);
        if (artifact.exists() && !artifact.remove()) removed = false;
    }
    return removed;
}

template<typename Initialize, typename ApplyInitialTarget, typename NotifyStart>
bool completeStart(Initialize initialize,
                   ApplyInitialTarget applyInitialTarget,
                   NotifyStart notifyStart)
{
    initialize();
    if (!applyInitialTarget()) return false;
    notifyStart();
    return true;
}

}

#endif
