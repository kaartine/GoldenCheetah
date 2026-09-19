/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_LOCALAPIENDPOINTINPUT_H
#define GC_LOCALAPIENDPOINTINPUT_H

#include "LocalApiFileStore.h"

#include <QByteArray>
#include <QDate>
#include <QString>
#include <memory>

namespace LocalApiEndpointInput {

enum class FileKind {
    RideDatabase,
    Activity,
    Cache,
    Zone,
    MeasuresSchema,
    MeasuresData
};

inline constexpr qint64 RideDatabaseMaximumSize = 128LL * 1024 * 1024;
inline constexpr qint64 ActivityMaximumSize = 64LL * 1024 * 1024;
inline constexpr qint64 CacheMaximumSize = 64LL * 1024 * 1024;
inline constexpr qint64 ZoneMaximumSize = 4LL * 1024 * 1024;
inline constexpr qint64 MeasuresSchemaMaximumSize = 1LL * 1024 * 1024;
inline constexpr qint64 MeasuresDataMaximumSize = 16LL * 1024 * 1024;
inline constexpr qint64 MeasuresMaximumResponseRows = 36600;
inline constexpr qint64 MeanMaxAggregateMaximumSize = 128LL * 1024 * 1024;
inline constexpr qsizetype AthleteDirectoryMaximumEntries = 1024;
inline constexpr qsizetype ActivityDirectoryMaximumEntries = 32768;
inline constexpr qsizetype MeanMaxCollectionMaximumPairs = 2048;
inline constexpr qint64 MeanMaxCollectionMaximumSize = 256LL * 1024 * 1024;
static_assert(
    ActivityMaximumSize + CacheMaximumSize
        <= MeanMaxAggregateMaximumSize,
    "The individual mean-max input budget must remain bounded");

qint64 maximumSize(FileKind kind);
bool fitsMeanMaxCollectionByteBudget(qint64 currentSize, qint64 addedSize);
bool prepareInclusiveDateRowCount(
    const QDate &start,
    const QDate &end,
    qint64 maximumRows,
    qint64 &rows);
bool isReadableMeasuresData(const QByteArray &contents);
bool prepareMeasuresDataFileNames(
    const QStringList &groupSymbols,
    QStringList &fileNames,
    QString &error);

enum class Status {
    Ready,
    Unavailable,
    InternalError
};

enum class Endpoint {
    RideDatabase,
    Activity,
    Zone,
    MeanMax,
    MeanMaxCollection
};

struct Contract
{
    int statusCode = 200;
    QByteArray bodyPrefix;
    bool processInput = false;
};

class PreparedInput
{
public:
    PreparedInput() = default;
    PreparedInput(PreparedInput &&) noexcept = default;
    PreparedInput &operator=(PreparedInput &&) noexcept = default;
    PreparedInput(const PreparedInput &) = delete;
    PreparedInput &operator=(const PreparedInput &) = delete;

    Status status() const { return status_; }
    const QByteArray &bytes() const;
    QString firstPath() const;
    QString secondPath() const;

private:
    Status status_ = Status::InternalError;
    QByteArray bytes_;
    QString firstPath_;
    QString secondPath_;
    std::unique_ptr<LocalApiFileSnapshotDirectory> firstSnapshot_;
    std::unique_ptr<LocalApiFileSnapshotDirectory> secondSnapshot_;

    friend PreparedInput prepareBytes(
        const LocalApiFileStore &,
        const AnchoredFileSystem::DirectoryAnchor &,
        const QString &, const QString &, FileKind, QString &);
    friend PreparedInput prepareSnapshot(
        const LocalApiFileStore &,
        const AnchoredFileSystem::DirectoryAnchor &,
        const QString &, const QString &, FileKind, QString &);
    friend PreparedInput prepareMeanMax(
        const LocalApiFileStore &,
        const AnchoredFileSystem::DirectoryAnchor &,
        const QString &, QString &);
    friend PreparedInput prepareMeanMaxCollection(
        const LocalApiFileStore &,
        const AnchoredFileSystem::DirectoryAnchor &,
        const QList<AnchoredFileSystem::DirectoryEntry> &, QString &);
    friend PreparedInput prepareOptionalSnapshotDirectory(
        const LocalApiFileStore &,
        const AnchoredFileSystem::DirectoryAnchor &,
        const QStringList &, const QString &, FileKind, QString &);
};

enum class ListingKind {
    RegularFiles,
    Directories
};

struct PreparedListing
{
    Status status = Status::InternalError;
    bool absent = false;
    QList<AnchoredFileSystem::DirectoryEntry> entries;
    QStringList names;
};

PreparedInput prepareBytes(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &athleteDirectory,
    const QString &directory,
    const QString &fileName,
    FileKind kind,
    QString &error);

PreparedInput prepareSnapshot(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &athleteDirectory,
    const QString &directory,
    const QString &fileName,
    FileKind kind,
    QString &error);

PreparedInput prepareMeanMax(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &athleteDirectory,
    const QString &activityFileName,
    QString &error);

PreparedInput prepareMeanMaxCollection(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &athleteDirectory,
    const QList<AnchoredFileSystem::DirectoryEntry> &activityEntries,
    QString &error);

// Produces a private directory in firstPath(). When the source exists its
// verified snapshot file is in secondPath(); absence leaves that path empty.
PreparedInput prepareOptionalSnapshotDirectory(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
    const QStringList &directoryComponents,
    const QString &fileName,
    FileKind kind,
    QString &error);

PreparedListing prepareListing(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
    const QStringList &directoryComponents,
    ListingKind kind,
    qsizetype maximumEntries,
    QString &error);

bool openListedDirectory(
    const LocalApiFileStore &store,
    const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
    const AnchoredFileSystem::DirectoryEntry &listedEntry,
    AnchoredFileSystem::DirectoryAnchor &directory,
    QString &error);

Contract listingContract(const PreparedListing &listing);

Contract contract(
    Endpoint endpoint,
    Status status,
    const QByteArray &series = QByteArray());

QString decodeRideDatabase(const QByteArray &contents);

} // namespace LocalApiEndpointInput

#endif
