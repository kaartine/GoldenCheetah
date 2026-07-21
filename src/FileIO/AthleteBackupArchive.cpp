/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "AthleteBackupArchive.h"

#include "AtomicFileWriter.h"
#include "../../contrib/qzip/zipreader.h"
#include "../../contrib/qzip/zipwriter.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QTemporaryFile>

#include <algorithm>
#include <limits>
#include <utility>

#include <zlib.h>

namespace {

constexpr qint64 backupChunkSize = 64 * 1024;
constexpr quint64 maximumClassicZipValue =
    static_cast<quint64>(std::numeric_limits<quint32>::max()) - 1U;

bool configureTrustedBackupLimits(
    ZipReader &reader,
    const AthleteBackupManifest &manifest)
{
    if (manifest.size() > std::numeric_limits<int>::max())
        return false;

    ArchiveResourceLimits limits;
    limits.maximumEntries =
        std::max(
            limits.maximumEntries,
            static_cast<int>(manifest.size()));
    limits.maximumCompressionRatio =
        std::numeric_limits<quint64>::max();

    QIODevice *archive = reader.device();
    if (!archive || archive->size() < 0)
        return false;
    const quint64 archiveSize =
        static_cast<quint64>(archive->size());
    limits.maximumCompressedSize =
        std::max(limits.maximumCompressedSize, archiveSize);
    limits.maximumMetadataSize =
        std::max(limits.maximumMetadataSize, archiveSize);

    quint64 totalSize = 0;
    for (const AthleteBackupArchiveEntry &entry : manifest) {
        if (entry.size < 0)
            return false;
        const quint64 size = static_cast<quint64>(entry.size);
        if (size > std::numeric_limits<quint64>::max() - totalSize)
            return false;
        totalSize += size;
        limits.maximumEntrySize =
            std::max(limits.maximumEntrySize, size);
    }
    limits.maximumTotalSize =
        std::max(limits.maximumTotalSize, totalSize);
    return reader.setResourceLimits(limits);
}

const QStringList &persistentDirectoryNames()
{
    static const QStringList names = {
        QStringLiteral("activities"),
        QStringLiteral("imports"),
        QStringLiteral("records"),
        QStringLiteral("downloads"),
        QStringLiteral("bak"),
        QStringLiteral("config"),
        QStringLiteral("calendar"),
        QStringLiteral("workouts"),
        QStringLiteral("media"),
        QStringLiteral("planned"),
        QStringLiteral("snippets"),
        QStringLiteral("quarantine")
    };
    return names;
}

bool openSource(
    const AthleteBackupSourceFactory &sourceFactory,
    const QString &sourcePath,
    std::unique_ptr<QIODevice> &source,
    bool &openedHere,
    QString &error)
{
    if (!sourceFactory) {
        error = QStringLiteral("No backup source reader is available");
        return false;
    }

    source = sourceFactory(sourcePath);
    if (!source) {
        error = QStringLiteral("Cannot create a reader for %1")
                    .arg(sourcePath);
        return false;
    }

    openedHere = false;
    if ((source->openMode() & QIODevice::ReadOnly) == 0) {
        if (source->isOpen() || !source->open(QIODevice::ReadOnly)) {
            error = QStringLiteral("Cannot open %1 for backup: %2")
                        .arg(sourcePath, source->errorString());
            return false;
        }
        openedHere = true;
    }
    return true;
}

bool readSourceMetadata(
    const QString &sourcePath,
    const AthleteBackupSourceFactory &sourceFactory,
    qint64 &size,
    quint32 &checksum,
    QString &error)
{
    std::unique_ptr<QIODevice> source;
    bool openedHere = false;
    if (!openSource(
            sourceFactory, sourcePath, source, openedHere, error)) {
        return false;
    }

    QByteArray buffer(static_cast<qsizetype>(backupChunkSize), Qt::Uninitialized);
    quint64 bytesRead = 0;
    uLong crc = ::crc32(0L, Z_NULL, 0);
    bool success = true;

    for (;;) {
        const qint64 count = source->read(buffer.data(), buffer.size());
        if (count < 0) {
            error = QStringLiteral("Cannot read %1 for backup: %2")
                        .arg(sourcePath, source->errorString());
            success = false;
            break;
        }
        if (count == 0) {
            if (!source->atEnd()) {
                error = QStringLiteral(
                    "Cannot read %1 for backup: the source stopped early")
                            .arg(sourcePath);
                success = false;
            }
            break;
        }
        if (bytesRead + static_cast<quint64>(count)
            > maximumClassicZipValue) {
            error = QStringLiteral(
                "Cannot back up %1: files larger than 4 GiB require ZIP64")
                        .arg(sourcePath);
            success = false;
            break;
        }

        bytesRead += static_cast<quint64>(count);
        crc = ::crc32(
            crc,
            reinterpret_cast<const Bytef *>(buffer.constData()),
            static_cast<uInt>(count));
    }

    if (openedHere) source->close();
    if (!success) return false;

    size = static_cast<qint64>(bytesRead);
    checksum = static_cast<quint32>(crc);
    return true;
}

bool appendFile(
    const QFileInfo &fileInfo,
    const QString &archivePath,
    const AthleteBackupSourceFactory &sourceFactory,
    AthleteBackupManifest &manifest,
    qint64 &totalSize,
    QString &error)
{
    if (fileInfo.isSymLink()) {
        error = QStringLiteral(
            "Cannot create a complete backup because %1 is a symbolic link")
                    .arg(fileInfo.absoluteFilePath());
        return false;
    }
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        error = QStringLiteral(
            "Cannot create a complete backup because %1 is not a regular file")
                    .arg(fileInfo.absoluteFilePath());
        return false;
    }

    qint64 size = 0;
    quint32 checksum = 0;
    if (!readSourceMetadata(
            fileInfo.absoluteFilePath(),
            sourceFactory,
            size,
            checksum,
            error)) {
        return false;
    }

    const QFileInfo afterRead(fileInfo.absoluteFilePath());
    if (!afterRead.exists()
        || !afterRead.isFile()
        || afterRead.isSymLink()
        || afterRead.size() != size) {
        error = QStringLiteral(
            "Cannot back up %1 because it changed while being read")
                    .arg(fileInfo.absoluteFilePath());
        return false;
    }
    if (size > std::numeric_limits<qint64>::max() - totalSize) {
        error = QStringLiteral("The backup size exceeds the supported limit");
        return false;
    }

    manifest.append({
        fileInfo.absoluteFilePath(),
        QDir::fromNativeSeparators(archivePath),
        size,
        checksum
    });
    totalSize += size;
    return true;
}

bool appendDirectory(
    const QString &directoryPath,
    const QString &archiveRoot,
    const AthleteBackupSourceFactory &sourceFactory,
    AthleteBackupManifest &manifest,
    qint64 &totalSize,
    QString &error)
{
    const QFileInfo directoryInfo(directoryPath);
    if (!directoryInfo.exists()) return true;
    if (directoryInfo.isSymLink()) {
        error = QStringLiteral(
            "Cannot create a complete backup because %1 is a symbolic link")
                    .arg(directoryPath);
        return false;
    }
    if (!directoryInfo.isDir() || !directoryInfo.isReadable()) {
        error = QStringLiteral(
            "Cannot read backup directory %1")
                    .arg(directoryPath);
        return false;
    }

    const QDir directory(directoryPath);
    const QFileInfoList children = directory.entryInfoList(
        QDir::AllEntries
            | QDir::NoDotAndDotDot
            | QDir::Hidden
            | QDir::System,
        QDir::DirsFirst | QDir::Name);

    for (const QFileInfo &child : children) {
        if (child.isSymLink()) {
            error = QStringLiteral(
                "Cannot create a complete backup because %1 is a symbolic link")
                        .arg(child.absoluteFilePath());
            return false;
        }

        const QString childArchivePath =
            archiveRoot + QLatin1Char('/') + child.fileName();
        if (child.isDir()) {
            if (!appendDirectory(
                    child.absoluteFilePath(),
                    childArchivePath,
                    sourceFactory,
                    manifest,
                    totalSize,
                    error)) {
                return false;
            }
        } else if (child.isFile()) {
            if (!appendFile(
                    child,
                    childArchivePath,
                    sourceFactory,
                    manifest,
                    totalSize,
                    error)) {
                return false;
            }
        } else {
            error = QStringLiteral(
                "Cannot create a complete backup because %1 is not a regular file")
                        .arg(child.absoluteFilePath());
            return false;
        }
    }
    return true;
}

QString zipStatusDescription(ZipWriter::Status status)
{
    switch (status) {
    case ZipWriter::NoError:
        return QStringLiteral("no error");
    case ZipWriter::FileWriteError:
        return QStringLiteral("write error");
    case ZipWriter::FileOpenError:
        return QStringLiteral("open error");
    case ZipWriter::FilePermissionsError:
        return QStringLiteral("permission error");
    case ZipWriter::FileReadError:
        return QStringLiteral("source read error");
    case ZipWriter::FileError:
        return QStringLiteral("file error");
    }
    return QStringLiteral("unknown error");
}

bool removePartiallyPublishedBackup(
    const QString &targetPath,
    bool targetPublished,
    QString &error)
{
    if (!targetPublished) return true;

    if (!QFile::remove(targetPath)) {
        if (!error.isEmpty()) error += QStringLiteral("; ");
        error += QStringLiteral(
            "cannot remove the partially published backup");
        return false;
    }

    QString syncError;
    if (!syncParentDirectory(targetPath, syncError)) {
        if (!error.isEmpty()) error += QStringLiteral("; ");
        error += syncError;
        return false;
    }
    return true;
}

} // namespace

AthleteBackupSourceFactory athleteBackupFileSourceFactory()
{
    return [](const QString &sourcePath) -> std::unique_ptr<QIODevice> {
        return std::make_unique<QFile>(sourcePath);
    };
}

bool buildAthleteBackupManifest(
    const QDir &athleteHome,
    const QDir &globalHome,
    AthleteBackupManifest &manifest,
    qint64 &totalSize,
    QString &error,
    AthleteBackupSourceFactory sourceFactory)
{
    manifest.clear();
    totalSize = 0;
    error.clear();

    const QFileInfo athleteInfo(athleteHome.absolutePath());
    if (!athleteInfo.exists()
        || !athleteInfo.isDir()
        || athleteInfo.isSymLink()
        || !athleteInfo.isReadable()) {
        error = QStringLiteral("The athlete directory cannot be read safely");
        return false;
    }

    for (const QString &directoryName : persistentDirectoryNames()) {
        if (!appendDirectory(
                athleteHome.filePath(directoryName),
                directoryName,
                sourceFactory,
                manifest,
                totalSize,
                error)) {
            manifest.clear();
            totalSize = 0;
            return false;
        }
    }

    static const QStringList databaseFiles = {
        QStringLiteral("trainDB"),
        QStringLiteral("trainDB-wal"),
        QStringLiteral("trainDB-shm"),
        QStringLiteral("trainDB-journal")
    };
    for (const QString &databaseName : databaseFiles) {
        const QFileInfo database(globalHome.filePath(databaseName));
        if (!database.exists() && !database.isSymLink()) continue;
        if (!appendFile(
                database,
                QStringLiteral("global/") + databaseName,
                sourceFactory,
                manifest,
                totalSize,
                error)) {
            manifest.clear();
            totalSize = 0;
            return false;
        }
    }

    std::sort(
        manifest.begin(),
        manifest.end(),
        [](const AthleteBackupArchiveEntry &left,
           const AthleteBackupArchiveEntry &right) {
            return left.archivePath < right.archivePath;
        });

    QSet<QString> archivePaths;
    for (const AthleteBackupArchiveEntry &entry : std::as_const(manifest)) {
        if (entry.archivePath.isEmpty()
            || archivePaths.contains(entry.archivePath)) {
            error = QStringLiteral(
                "The backup manifest contains a duplicate or empty path");
            manifest.clear();
            totalSize = 0;
            return false;
        }
        archivePaths.insert(entry.archivePath);
    }

    if (manifest.size() >= std::numeric_limits<quint16>::max()) {
        error = QStringLiteral(
            "The backup contains too many files for the ZIP format");
        manifest.clear();
        totalSize = 0;
        return false;
    }
    return true;
}

bool writeAthleteBackupArchive(
    const QString &archivePath,
    const AthleteBackupManifest &manifest,
    QString &error,
    AthleteBackupSourceFactory sourceFactory,
    AthleteBackupProgressFunction progress)
{
    error.clear();
    if (manifest.isEmpty()) {
        error = QStringLiteral("The backup manifest is empty");
        return false;
    }

    ZipWriter writer(archivePath);
    if (writer.status() != ZipWriter::NoError) {
        error = QStringLiteral("Cannot create the backup archive: %1")
                    .arg(zipStatusDescription(writer.status()));
        return false;
    }

    const int totalFiles = manifest.size();
    int completedFiles = 0;
    if (progress && !progress(completedFiles, totalFiles)) {
        writer.close();
        error = QStringLiteral("Backup canceled");
        return false;
    }

    for (const AthleteBackupArchiveEntry &entry : manifest) {
        std::unique_ptr<QIODevice> source;
        bool openedHere = false;
        if (!openSource(
                sourceFactory,
                entry.sourcePath,
                source,
                openedHere,
                error)) {
            writer.close();
            return false;
        }

        writer.addFile(entry.archivePath, source.get());
        if (openedHere && source->isOpen()) source->close();
        if (writer.status() != ZipWriter::NoError) {
            error = QStringLiteral(
                "Cannot add %1 to the backup archive: %2")
                        .arg(
                            entry.archivePath,
                            zipStatusDescription(writer.status()));
            writer.close();
            return false;
        }

        ++completedFiles;
        if (progress && !progress(completedFiles, totalFiles)) {
            writer.close();
            error = QStringLiteral("Backup canceled");
            return false;
        }
    }

    writer.close();
    if (writer.status() != ZipWriter::NoError) {
        error = QStringLiteral("Cannot finish the backup archive: %1")
                    .arg(zipStatusDescription(writer.status()));
        return false;
    }
    return true;
}

bool verifyAthleteBackupArchive(
    const QString &archivePath,
    const AthleteBackupManifest &manifest,
    QString &error)
{
    error.clear();
    ZipReader reader(archivePath);
    if (!configureTrustedBackupLimits(reader, manifest)) {
        error = QStringLiteral("Cannot configure backup verification");
        return false;
    }
    const QList<ZipReader::FileInfo> files = reader.fileInfoList();
    if (reader.status() != ZipReader::NoError) {
        error = QStringLiteral("Cannot verify the backup archive");
        return false;
    }
    if (files.size() != manifest.size()) {
        error = QStringLiteral(
            "The backup archive does not match its manifest");
        return false;
    }

    QHash<QString, const AthleteBackupArchiveEntry *> expected;
    for (const AthleteBackupArchiveEntry &entry : manifest) {
        if (expected.contains(entry.archivePath)) {
            error = QStringLiteral(
                "The backup manifest contains duplicate paths");
            return false;
        }
        expected.insert(entry.archivePath, &entry);
    }

    QSet<QString> observed;
    for (const ZipReader::FileInfo &file : files) {
        const auto match = expected.constFind(file.filePath);
        if (!file.isFile
            || file.isDir
            || file.isSymLink
            || match == expected.constEnd()
            || observed.contains(file.filePath)
            || file.size != (*match)->size
            || file.crc_32 != (*match)->crc32
            || !reader.verifyFile(file.filePath)) {
            error = QStringLiteral(
                "The backup archive does not match its manifest");
            return false;
        }
        observed.insert(file.filePath);
    }

    if (observed.size() != expected.size()) {
        error = QStringLiteral(
            "The backup archive does not match its manifest");
        return false;
    }
    return true;
}

bool publishVerifiedAthleteBackup(
    const QString &targetPath,
    const AthleteBackupManifest &manifest,
    QString &error,
    AthleteBackupSourceFactory sourceFactory,
    AthleteBackupProgressFunction progress,
    AthleteBackupArchiveBuildFunction archiveBuilder)
{
    error.clear();
    if (manifest.isEmpty()) {
        error = QStringLiteral("The backup manifest is empty");
        return false;
    }

    const QFileInfo target(targetPath);
    if (target.exists() || target.isSymLink()) {
        error = QStringLiteral("The backup target already exists");
        return false;
    }
    const QDir targetDirectory(target.absolutePath());
    if (!targetDirectory.exists()) {
        error = QStringLiteral("The backup directory does not exist");
        return false;
    }

    QTemporaryFile staging(targetDirectory.filePath(
        QStringLiteral(".%1.XXXXXX.tmp").arg(target.fileName())));
    if (!staging.open()) {
        error = QStringLiteral("Cannot create a temporary backup file: %1")
                    .arg(staging.errorString());
        return false;
    }
    const QString stagingPath = staging.fileName();
    staging.close();

    if (!archiveBuilder) {
        archiveBuilder =
            [](const QString &path,
               const AthleteBackupManifest &entries,
               const AthleteBackupSourceFactory &factory,
               const AthleteBackupProgressFunction &callback,
               QString &buildError) {
                return writeAthleteBackupArchive(
                    path, entries, buildError, factory, callback);
            };
    }
    if (!archiveBuilder(
            stagingPath,
            manifest,
            sourceFactory,
            progress,
            error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("Cannot build the backup archive");
        }
        return false;
    }

    QString verificationError;
    if (!verifyAthleteBackupArchive(
            stagingPath, manifest, verificationError)) {
        error = verificationError.isEmpty()
            ? QStringLiteral("Cannot verify the backup archive")
            : verificationError;
        return false;
    }

    QFile stagedFile(stagingPath);
    if (!stagedFile.open(QIODevice::ReadWrite)) {
        error = QStringLiteral("Cannot reopen the backup archive: %1")
                    .arg(stagedFile.errorString());
        return false;
    }
    QString syncError;
    if (!syncFileDevice(stagedFile, syncError)) {
        stagedFile.close();
        error = QStringLiteral("Cannot sync the backup archive: %1")
                    .arg(syncError);
        return false;
    }
    stagedFile.close();

    bool targetPublished = false;
    QString publishError;
    if (!publishAtomicNew(
            stagingPath,
            targetPath,
            targetPublished,
            publishError)) {
        error = QStringLiteral("Cannot publish the backup archive: %1")
                    .arg(publishError);
        removePartiallyPublishedBackup(
            targetPath, targetPublished, error);
        return false;
    }
    staging.setAutoRemove(false);

    if (!syncParentDirectory(targetPath, syncError)) {
        error = QStringLiteral("Cannot sync the backup directory: %1")
                    .arg(syncError);
        removePartiallyPublishedBackup(targetPath, true, error);
        return false;
    }
    return true;
}
