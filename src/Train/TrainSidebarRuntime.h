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

#include <algorithm>
#include <cmath>

namespace TrainSidebarRuntime
{

inline long contextWorkoutPosition(double positionMeters, bool finished)
{
    return long(finished
            ? std::ceil(positionMeters) : std::floor(positionMeters));
}

inline double courseFtpWatts(double athleteFtpWatts,
                             double scaledWorkoutFtpWatts,
                             double declaredWorkoutFtpWatts)
{
    if (std::isfinite(scaledWorkoutFtpWatts) && scaledWorkoutFtpWatts > 0.0)
        return scaledWorkoutFtpWatts;
    if (std::isfinite(athleteFtpWatts) && athleteFtpWatts > 0.0)
        return athleteFtpWatts;
    if (std::isfinite(declaredWorkoutFtpWatts)
            && declaredWorkoutFtpWatts > 0.0)
        return declaredWorkoutFtpWatts;
    return 0.0;
}

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

inline double slopeTarget(double currentSlope,
                          double workoutSlope,
                          bool initialize)
{
    return initialize ? workoutSlope : currentSlope;
}

inline double workoutLapPositionMeters(
        bool generatedCourse,
        double timelinePositionMeters,
        double visualPositionMeters)
{
    return generatedCourse ? timelinePositionMeters : visualPositionMeters;
}

template<typename AddLap>
int insertManualLap(bool generatedCourse,
                    double timelinePositionMeters,
                    double visualPositionMeters,
                    AddLap addLap)
{
    return addLap(workoutLapPositionMeters(
            generatedCourse,
            timelinePositionMeters,
            visualPositionMeters));
}

enum class CueTimelineReset {
    WorkoutStart,
    LapChange,
    Seek
};

inline double resetCuePosition(CueTimelineReset reason,
                               double previousEnd,
                               double currentPosition)
{
    if (reason == CueTimelineReset::WorkoutStart) return -1.0;

    const double coveredEnd = std::isfinite(previousEnd)
            ? previousEnd : -1.0;
    if (reason == CueTimelineReset::LapChange) return coveredEnd;
    if (!std::isfinite(currentPosition)) return coveredEnd;

    return std::max(coveredEnd, std::max(0.0, currentPosition));
}

struct CueSearchWindow
{
    bool ready = false;
    double start = 0.0;
    double end = 0.0;
};

inline CueSearchWindow cueSearchWindow(
        double previousEnd,
        double currentPosition,
        double lookAhead)
{
    CueSearchWindow result;
    if (!std::isfinite(previousEnd) || !std::isfinite(currentPosition)
            || !std::isfinite(lookAhead) || lookAhead < 0.0) {
        return result;
    }
    result.start = previousEnd < 0.0 ? 0.0 : previousEnd;
    result.end = std::max(0.0, currentPosition) + lookAhead;
    result.ready = result.end >= result.start;
    return result;
}

inline bool cueIsNewInWindow(
        const CueSearchWindow &window,
        double previousEnd,
        double cuePosition)
{
    return window.ready && std::isfinite(cuePosition)
            && cuePosition > previousEnd
            && cuePosition >= window.start
            && cuePosition <= window.end;
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
