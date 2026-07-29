/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDECACHESAVESNAPSHOT_H
#define GC_RIDECACHESAVESNAPSHOT_H

#include "FileIO/AtomicFileWriter.h"

#include <QByteArray>
#include <QDateTime>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace RideCacheSave {

struct MetricDefinition
{
    QString name;
    int index = -1;
    bool aggregateZero = false;
};

struct MetricValues
{
    QVector<double> values;
    QVector<double> counts;
    QMap<int, double> stdMeans;
    QMap<int, double> stdVariances;
};

struct Interval
{
    QString name;
    double start = 0;
    double stop = 0;
    double startKm = 0;
    double stopKm = 0;
    int type = 0;
    bool test = false;
    QString color;
    QString route;
    int sequence = 0;
    MetricValues metricValues;
};

struct Ride
{
    QDateTime dateTime;
    QString fileName;
    quint64 fingerprint = 0;
    quint64 crc = 0;
    quint64 metadataCrc = 0;
    quint64 timestamp = 0;
    int databaseVersion = 0;
    int userDatabaseVersion = 0;
    QString color;
    QString present;
    QString sport;
    bool aero = false;
    double weight = 0;
    int zoneRange = -1;
    int hrZoneRange = -1;
    int paceZoneRange = -1;
    QStringList overrides;
    bool samples = false;
    MetricValues metricValues;
    QMap<QString, QString> metadata;
    QMap<QString, QStringList> xdata;
    QVector<Interval> intervals;
};

struct Snapshot
{
    QString version;
    QString targetPath;
    QVector<MetricDefinition> metrics;
    QVector<Ride> rides;
};

bool deferTarget(
    QStringList &pendingTargets,
    bool refreshActive,
    const QString &targetPath);

QStringList takeDeferredTargets(QStringList &pendingTargets);

QStringList takeDeferredTargetsForCancellation(
    QStringList &pendingTargets,
    const QString &defaultTarget,
    bool exiting,
    bool cacheComplete);

bool serialize(
    const Snapshot &snapshot,
    QByteArray &document,
    QString &error);

bool write(
    const Snapshot &snapshot,
    QString &error,
    const AtomicFileWriterFactory &writerFactory =
        qSaveFileWriterFactory());

} // namespace RideCacheSave

#endif // GC_RIDECACHESAVESNAPSHOT_H
