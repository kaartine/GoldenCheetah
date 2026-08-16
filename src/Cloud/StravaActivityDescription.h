/*
 * Copyright (c) 2026 Jukka Kaartinen
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_STRAVA_ACTIVITY_DESCRIPTION_H
#define GC_STRAVA_ACTIVITY_DESCRIPTION_H

#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <QVector>

class StravaActivityDescription
{
    Q_DECLARE_TR_FUNCTIONS(StravaActivityDescription)

public:
    enum class Mode {
        NotesOnly,
        SummaryOnly,
        NotesAndSummary
    };

    struct Sample {
        double seconds = 0.0;
        double power = 0.0;
        double heartRate = 0.0;
        double cadence = 0.0;
    };

    struct Interval {
        double start = 0.0;
        double stop = 0.0;
        QString name;
    };

    struct Zone {
        QString name;
        double high = 0.0;
    };

    struct Input {
        QVector<Sample> samples;
        QVector<Interval> intervals;
        QVector<Zone> powerZones;
        double recordingInterval = 1.0;
        double ftp = 0.0;
        double duration = 0.0;
        double work = 0.0;
        double averagePower = 0.0;
        double maximumPower = 0.0;
        double normalizedPower = 0.0;
        double intensityFactor = 0.0;
        double bikeStress = 0.0;
        double averageHeartRate = 0.0;
        double maximumHeartRate = 0.0;
        double averageCadence = 0.0;
        bool hasPower = false;
        bool hasHeartRate = false;
        bool hasCadence = false;
    };

    struct RemoteUpdate {
        bool valid = false;
        bool changed = false;
        QByteArray requestBody;
        QString error;
    };

    static Mode modeFromSetting(const QString &setting);
    static QString summary(const Input &input);
    static QString managedSummaryBlock(const QString &automaticSummary);
    static QString mergeManagedSummary(
        const QString &remoteDescription,
        const QString &automaticSummary);
    static RemoteUpdate prepareRemoteUpdate(
        const QByteArray &activityResponse,
        const QString &automaticSummary);
    static QString compose(
        const QString &notes,
        bool notesUsedAsActivityName,
        Mode mode,
        const QString &automaticSummary);
};

#endif
