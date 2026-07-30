/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_OPEN_DATA_CAPTURE_UTILS_H
#define GC_OPEN_DATA_CAPTURE_UTILS_H

#include <QByteArray>
#include <QString>
#include <QtTypes>

#include <functional>
#include <memory>

class QFile;
class QIODevice;

namespace OpenDataCaptureUtils {

struct ActivityFileIdentity
{
    quint64 volume = 0;
    quint64 file = 0;
    qint64 size = -1;
    quint64 modified = 0;
    quint64 modifiedFraction = 0;
    QByteArray sha256;
    bool valid = false;

    bool operator==(const ActivityFileIdentity &other) const;
};

struct ActivitySeries
{
    bool power = false;
    bool heartRate = false;
    bool cadence = false;
    bool altitude = false;
};

struct ActivitySample
{
    double secs = 0;
    double km = 0;
    double power = 0;
    double heartRate = 0;
    double cadence = 0;
    double altitude = 0;
};

struct SnapshotCaptureOperations
{
    std::function<bool(QString &error)> settleRefresh;
    std::function<bool(QString &error)> captureManifest;
    std::function<bool(QString &error)> writeSummary;
};

bool captureManifestThenSummary(
    const SnapshotCaptureOperations &operations,
    QString &error);

QString jsonStringLiteral(const QString &value);
QString activitySourcePath(
    const QString &allowedRoot,
    const QString &directory,
    const QString &fileName);
std::unique_ptr<QFile> openActivitySource(
    const QString &allowedRoot,
    const QString &validatedPath,
    QString &error);
std::unique_ptr<QFile> openActivitySource(
    const QString &allowedRoot,
    const QString &validatedPath,
    const ActivityFileIdentity *expectedIdentity,
    ActivityFileIdentity *actualIdentity,
    QString &error);
QString activitySnapshotTemplate(const QString &fileName);
bool includeActivityInSnapshot(
    bool hasMetrics,
    bool skipSave);
QString activityArchiveName(const QString &fileName);
bool copyActivitySnapshot(
    QIODevice *source,
    QIODevice *destination,
    QString &error);
bool copyActivitySnapshot(
    QIODevice *source,
    QIODevice *destination,
    const QByteArray &expectedSha256,
    QString &error);
bool writeCsvHeader(QIODevice *destination, QString &error);
bool writeCsvSample(
    QIODevice *destination,
    const ActivitySeries &series,
    const ActivitySample &sample,
    QString &error);

} // namespace OpenDataCaptureUtils

#endif
