/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "LocalApiEndpointInput.h"
#include "PortableFileName.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QTextStream>
#include <algorithm>

namespace LocalApiEndpointInput {

qint64 maximumSize(FileKind kind)
{
    switch (kind) {
    case FileKind::RideDatabase:
        return RideDatabaseMaximumSize;
    case FileKind::Activity:
        return ActivityMaximumSize;
    case FileKind::Cache:
        return CacheMaximumSize;
    case FileKind::Zone:
        return ZoneMaximumSize;
    case FileKind::MeasuresSchema:
        return MeasuresSchemaMaximumSize;
    case FileKind::MeasuresData:
        return MeasuresDataMaximumSize;
    }
    return -1;
}

bool fitsMeanMaxCollectionByteBudget(
    qint64 currentSize, qint64 addedSize)
{
    return currentSize >= 0
        && addedSize >= 0
        && currentSize <= MeanMaxCollectionMaximumSize
        && addedSize <= MeanMaxCollectionMaximumSize - currentSize;
}

bool prepareInclusiveDateRowCount(
    const QDate &start,
    const QDate &end,
    qint64 maximumRows,
    qint64 &rows)
{
    rows = 0;
    if (maximumRows < 0) return false;
    if (!start.isValid() || !end.isValid() || start > end) return true;
    const qint64 days = start.daysTo(end);
    if (days < 0 || days >= maximumRows) return false;
    rows = days + 1;
    return true;
}

bool isReadableMeasuresData(const QByteArray &contents)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(contents, &parseError);
    return parseError.error == QJsonParseError::NoError
        && !document.isEmpty()
        && !document.isNull();
}

bool prepareMeasuresDataFileNames(
    const QStringList &groupSymbols,
    QStringList &fileNames,
    QString &error)
{
    fileNames.clear();
    error.clear();
    fileNames.reserve(groupSymbols.size());
    for (const QString &symbol : groupSymbols) {
        const QString fileName =
            symbol.toLower() + QStringLiteral("measures.json");
        if (!PortableFileName::isValid(fileName)) {
            fileNames.clear();
            error = QStringLiteral(
                "A measures group produces an unsafe data filename");
            return false;
        }
        fileNames.append(fileName);
    }
    return true;
}

bool prepareRideItemActivityPath(
    const AnchoredFileSystem::DirectoryAnchor &athleteDirectory,
    QString &activityPath,
    QString &error)
{
    activityPath.clear();
    error.clear();
    if (!athleteDirectory.isValid()) {
        error = QStringLiteral("The athlete directory is unavailable");
        return false;
    }
    const auto pathStillMatches = [&athleteDirectory, &error]() {
        if (athleteDirectory.pathMatches(error)) return true;
        if (error.isEmpty()) {
            error = QStringLiteral("The athlete directory path changed");
        }
        return false;
    };
    if (!pathStillMatches()) return false;

    const QString preparedPath = QDir(
        athleteDirectory.displayPath()).filePath(
            QStringLiteral("activities"));
    if (!pathStillMatches()) return false;

    activityPath = preparedPath;
    return true;
}

const QByteArray &PreparedInput::bytes() const
{
    static const QByteArray empty;
    return status_ == Status::Ready ? bytes_ : empty;
}

QString PreparedInput::firstPath() const
{
    return status_ == Status::Ready ? firstPath_ : QString();
}

QString PreparedInput::secondPath() const
{
    return status_ == Status::Ready ? secondPath_ : QString();
}

PreparedInput prepareBytes(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &athleteDirectory,
    const QString &directory,
    const QString &fileName,
    FileKind kind,
    QString &error)
{
    error.clear();
    PreparedInput result;
    LocalApiFileGeneration generation;
    if (!store.captureRegularFile(
            athleteDirectory, {directory}, fileName,
            generation, error, maximumSize(kind))
        || !generation.readAll(result.bytes_, error)) {
        result.bytes_.clear();
        result.status_ = Status::Unavailable;
        return result;
    }
    result.status_ = Status::Ready;
    return result;
}

PreparedInput prepareSnapshot(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &athleteDirectory,
    const QString &directory,
    const QString &fileName,
    FileKind kind,
    QString &error)
{
    PreparedInput result = prepareBytes(
        store, athleteDirectory, directory, fileName, kind, error);
    if (result.status_ != Status::Ready) return result;

    result.firstSnapshot_ =
        std::make_unique<LocalApiFileSnapshotDirectory>();
    if (!result.firstSnapshot_->writeFile(
            fileName, result.bytes_, result.firstPath_, error)) {
        result = {};
        result.status_ = Status::InternalError;
        return result;
    }
    result.bytes_.clear();
    return result;
}

PreparedInput prepareMeanMax(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &athleteDirectory,
    const QString &activityFileName,
    QString &error)
{
    const QString cacheName =
        QFileInfo(activityFileName).completeBaseName()
        + QStringLiteral(".cpx");
    PreparedInput source = prepareBytes(
        store, athleteDirectory, QStringLiteral("activities"),
        activityFileName, FileKind::Activity, error);
    if (source.status_ != Status::Ready) return source;
    PreparedInput cache = prepareBytes(
        store, athleteDirectory, QStringLiteral("cache"),
        cacheName, FileKind::Cache, error);
    if (cache.status_ != Status::Ready) {
        source.bytes_.clear();
        source.status_ = Status::Unavailable;
        return source;
    }

    source.firstSnapshot_ =
        std::make_unique<LocalApiFileSnapshotDirectory>();
    source.secondSnapshot_ =
        std::make_unique<LocalApiFileSnapshotDirectory>();
    if (!source.firstSnapshot_->writeFile(
            activityFileName, source.bytes_, source.firstPath_, error)
        || !source.secondSnapshot_->writeFile(
            cacheName, cache.bytes_, source.secondPath_, error)) {
        source = {};
        source.status_ = Status::InternalError;
        return source;
    }
    source.bytes_.clear();
    cache.bytes_.clear();
    source.status_ = Status::Ready;
    return source;
}

PreparedListing prepareListing(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
    const QStringList &directoryComponents,
    ListingKind kind,
    qsizetype maximumEntries,
    QString &error)
{
    Q_UNUSED(store)
    error.clear();
    PreparedListing result;
    if (maximumEntries < 0) {
        error = QStringLiteral(
            "The Local API directory entry budget is invalid");
        result.status = Status::Unavailable;
        return result;
    }

    AnchoredFileSystem::DirectoryAnchor directory = baseDirectory;
    QList<AnchoredFileSystem::DirectoryEntry> entries;
    if (!directory.isValid() || !directory.pathMatches(error)) {
        result.status = Status::Unavailable;
        return result;
    }
    for (const QString &component : directoryComponents) {
        AnchoredFileSystem::DirectoryAnchor child;
        bool exists = false;
        if (!directory.openChildIfExists(
                component, child, exists, error)) {
            result.status = Status::Unavailable;
            return result;
        }
        if (!exists) {
            result.status = Status::Unavailable;
            result.absent = true;
            return result;
        }
        directory = std::move(child);
    }
    if (!directory.enumerateEntries(entries, maximumEntries, error)) {
        result.status = Status::Unavailable;
        return result;
    }

    const AnchoredFileSystem::DirectoryEntryKind expectedKind =
        kind == ListingKind::RegularFiles
        ? AnchoredFileSystem::DirectoryEntryKind::RegularFile
        : AnchoredFileSystem::DirectoryEntryKind::Directory;
    for (const AnchoredFileSystem::DirectoryEntry &entry : entries) {
        if (entry.kind == expectedKind && !entry.hidden) {
            result.entries.append(entry);
        }
    }
    std::sort(
        result.entries.begin(), result.entries.end(),
        [](const AnchoredFileSystem::DirectoryEntry &left,
           const AnchoredFileSystem::DirectoryEntry &right) {
            return left.name < right.name;
        });
    for (const AnchoredFileSystem::DirectoryEntry &entry : result.entries) {
        result.names.append(entry.name);
    }
    result.status = Status::Ready;
    return result;
}

bool openListedDirectory(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
    const AnchoredFileSystem::DirectoryEntry &listedEntry,
    AnchoredFileSystem::DirectoryAnchor &directory,
    QString &error)
{
    directory = {};
    error.clear();
    AnchoredFileSystem::DirectoryAnchor candidate;
    if (listedEntry.kind
            != AnchoredFileSystem::DirectoryEntryKind::Directory
        || !store.openDirectory(
            baseDirectory, {listedEntry.name}, candidate, error)
        || candidate.identity() != listedEntry.identity) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The listed Local API directory generation changed");
        }
        return false;
    }
    directory = std::move(candidate);
    return true;
}

Contract listingContract(const PreparedListing &listing)
{
    Contract result;
    if (listing.status == Status::Ready) {
        result.processInput = true;
    } else if (!listing.absent) {
        result.statusCode = 500;
        result.bodyPrefix = QByteArrayLiteral(
            "unable to enumerate activities safely.\n");
    }
    return result;
}

PreparedInput prepareMeanMaxCollection(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &athleteDirectory,
    const QList<AnchoredFileSystem::DirectoryEntry> &activityEntries,
    QString &error)
{
    error.clear();
    PreparedInput result;
    struct Pair {
        AnchoredFileSystem::DirectoryEntry activity;
        AnchoredFileSystem::DirectoryEntry cache;
    };
    QList<Pair> pairs;
    if (!activityEntries.isEmpty()) {
        const PreparedListing caches = prepareListing(
            store, athleteDirectory, {QStringLiteral("cache")},
            ListingKind::RegularFiles, ActivityDirectoryMaximumEntries,
            error);
        if (caches.status != Status::Ready) {
            if (caches.absent) {
                result.status_ = Status::Unavailable;
            }
            return result;
        }
        QHash<QString, AnchoredFileSystem::DirectoryEntry> cachesByName;
        for (const AnchoredFileSystem::DirectoryEntry &cache : caches.entries) {
            cachesByName.insert(cache.name, cache);
        }
        for (const AnchoredFileSystem::DirectoryEntry &activity
             : activityEntries) {
            const QString basename = QFileInfo(activity.name).baseName();
            const QString cacheName = basename + QStringLiteral(".cpx");
            if (basename.isEmpty()
                || !cachesByName.contains(cacheName)) {
                continue;
            }
            pairs.append({activity, cachesByName.value(cacheName)});
            if (pairs.size() > MeanMaxCollectionMaximumPairs) {
                error = QStringLiteral(
                    "The mean-max collection exceeds its pair budget");
                result.status_ = Status::InternalError;
                return result;
            }
        }
    }
    result.firstSnapshot_ =
        std::make_unique<LocalApiFileSnapshotDirectory>();
    result.secondSnapshot_ =
        std::make_unique<LocalApiFileSnapshotDirectory>();
    if (!result.firstSnapshot_->isValid()
        || !result.secondSnapshot_->isValid()) {
        error = QStringLiteral(
            "Cannot create private mean-max collection directories");
        result = {};
        result.status_ = Status::InternalError;
        return result;
    }

    qint64 totalSize = 0;
    QSet<QString> snappedCacheNames;
    for (const Pair &pair : pairs) {
        LocalApiFileGeneration activityGeneration;
        QByteArray activityBytes;
        if (!store.captureListedRegularFile(
                athleteDirectory, {QStringLiteral("activities")},
                pair.activity, activityGeneration, error,
                maximumSize(FileKind::Activity))
            || !activityGeneration.readAll(activityBytes, error)
            || !fitsMeanMaxCollectionByteBudget(
                totalSize, activityBytes.size())) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The mean-max collection exceeds its byte budget");
            }
            result = {};
            result.status_ = Status::InternalError;
            return result;
        }
        totalSize += activityBytes.size();
        QString activityPath;
        if (!result.firstSnapshot_->writeFile(
                pair.activity.name, activityBytes, activityPath, error)) {
            result = {};
            result.status_ = Status::InternalError;
            return result;
        }
        if (!snappedCacheNames.contains(pair.cache.name)) {
            LocalApiFileGeneration cacheGeneration;
            QByteArray cacheBytes;
            if (!store.captureListedRegularFile(
                    athleteDirectory, {QStringLiteral("cache")},
                    pair.cache, cacheGeneration, error,
                    maximumSize(FileKind::Cache))
                || !cacheGeneration.readAll(cacheBytes, error)
                || !fitsMeanMaxCollectionByteBudget(
                    totalSize, cacheBytes.size())) {
                if (error.isEmpty()) {
                    error = QStringLiteral(
                        "The mean-max collection exceeds its byte budget");
                }
                result = {};
                result.status_ = Status::InternalError;
                return result;
            }
            totalSize += cacheBytes.size();
            QString cachePath;
            if (!result.secondSnapshot_->writeFile(
                    pair.cache.name, cacheBytes, cachePath, error)) {
                result = {};
                result.status_ = Status::InternalError;
                return result;
            }
            snappedCacheNames.insert(pair.cache.name);
        }
    }
    result.firstPath_ = result.firstSnapshot_->path();
    result.secondPath_ = result.secondSnapshot_->path();
    result.status_ = Status::Ready;
    return result;
}

PreparedInput prepareOptionalSnapshotDirectory(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
    const QStringList &directoryComponents,
    const QString &fileName,
    FileKind kind,
    QString &error)
{
    error.clear();
    PreparedInput result;
    LocalApiFileGeneration generation;
    bool exists = false;
    if (!store.captureRegularFileIfExists(
            baseDirectory, directoryComponents, fileName,
            generation, exists, error, maximumSize(kind))) {
        result.status_ = Status::InternalError;
        return result;
    }

    QByteArray contents;
    if (exists && !generation.readAll(contents, error)) {
        result.status_ = Status::InternalError;
        return result;
    }
    result.firstSnapshot_ =
        std::make_unique<LocalApiFileSnapshotDirectory>();
    if (!result.firstSnapshot_->isValid()) {
        error = QStringLiteral(
            "Cannot create private optional-input directory");
        result = {};
        result.status_ = Status::InternalError;
        return result;
    }
    result.firstPath_ = result.firstSnapshot_->path();
    if (exists && !result.firstSnapshot_->writeFile(
            fileName, contents, result.secondPath_, error)) {
        result = {};
        result.status_ = Status::InternalError;
        return result;
    }
    result.status_ = Status::Ready;
    return result;
}

Contract contract(
    Endpoint endpoint,
    Status status,
    const QByteArray &series)
{
    Contract result;
    if (status == Status::Ready) {
        result.processInput = true;
        if (endpoint == Endpoint::MeanMax
            || endpoint == Endpoint::MeanMaxCollection) {
            result.bodyPrefix = QByteArrayLiteral("secs, ")
                + series + QByteArrayLiteral("\n");
        }
        return result;
    }

    if (status == Status::InternalError) {
        result.statusCode = 500;
        if (endpoint == Endpoint::Activity) {
            result.bodyPrefix = QByteArrayLiteral(
                "unable to create a private input snapshot");
        } else if (endpoint == Endpoint::MeanMax) {
            result.bodyPrefix = QByteArrayLiteral(
                "unable to create private mean-max snapshots\n");
        } else if (endpoint == Endpoint::MeanMaxCollection) {
            result.bodyPrefix = QByteArrayLiteral(
                "unable to prepare mean-max collection safely\n");
        }
        return result;
    }

    switch (endpoint) {
    case Endpoint::RideDatabase:
        result.statusCode = 404;
        result.bodyPrefix = QByteArrayLiteral(
            "malformed URL or unknown athlete.\n");
        break;
    case Endpoint::Activity:
        result.statusCode = 404;
        result.bodyPrefix = QByteArrayLiteral(
            "file not found or unsafe");
        break;
    case Endpoint::Zone:
        result.statusCode = 500;
        break;
    case Endpoint::MeanMax:
    case Endpoint::MeanMaxCollection:
        result.bodyPrefix = QByteArrayLiteral("secs, ")
            + series + QByteArrayLiteral("\n");
        break;
    }
    return result;
}

QString decodeRideDatabase(const QByteArray &contents)
{
    QByteArray copy = contents;
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    QTextStream stream(&buffer);
    return stream.readAll();
}

} // namespace LocalApiEndpointInput
