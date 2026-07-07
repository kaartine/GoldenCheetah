/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_AthleteBackupArchive_h
#define _GC_AthleteBackupArchive_h

#include <QDir>
#include <QIODevice>
#include <QList>
#include <QString>

#include <functional>
#include <memory>

struct AthleteBackupArchiveEntry
{
    QString sourcePath;
    QString archivePath;
    qint64 size = 0;
    quint32 crc32 = 0;
};

using AthleteBackupManifest = QList<AthleteBackupArchiveEntry>;
using AthleteBackupSourceFactory =
    std::function<std::unique_ptr<QIODevice>(const QString &sourcePath)>;
using AthleteBackupProgressFunction =
    std::function<bool(int completedFiles, int totalFiles)>;
using AthleteBackupArchiveBuildFunction = std::function<bool(
    const QString &archivePath,
    const AthleteBackupManifest &manifest,
    const AthleteBackupSourceFactory &sourceFactory,
    const AthleteBackupProgressFunction &progress,
    QString &error)>;

AthleteBackupSourceFactory athleteBackupFileSourceFactory();

bool buildAthleteBackupManifest(
    const QDir &athleteHome,
    const QDir &globalHome,
    AthleteBackupManifest &manifest,
    qint64 &totalSize,
    QString &error,
    AthleteBackupSourceFactory sourceFactory =
        athleteBackupFileSourceFactory());

bool writeAthleteBackupArchive(
    const QString &archivePath,
    const AthleteBackupManifest &manifest,
    QString &error,
    AthleteBackupSourceFactory sourceFactory =
        athleteBackupFileSourceFactory(),
    AthleteBackupProgressFunction progress = {});

bool verifyAthleteBackupArchive(
    const QString &archivePath,
    const AthleteBackupManifest &manifest,
    QString &error);

bool publishVerifiedAthleteBackup(
    const QString &targetPath,
    const AthleteBackupManifest &manifest,
    QString &error,
    AthleteBackupSourceFactory sourceFactory =
        athleteBackupFileSourceFactory(),
    AthleteBackupProgressFunction progress = {},
    AthleteBackupArchiveBuildFunction archiveBuilder = {});

#endif
