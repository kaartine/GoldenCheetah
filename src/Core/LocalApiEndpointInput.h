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
#include <QString>
#include <memory>

namespace LocalApiEndpointInput {

enum class FileKind {
    RideDatabase,
    Activity,
    Cache,
    Zone
};

inline constexpr qint64 RideDatabaseMaximumSize = 128LL * 1024 * 1024;
inline constexpr qint64 ActivityMaximumSize = 64LL * 1024 * 1024;
inline constexpr qint64 CacheMaximumSize = 64LL * 1024 * 1024;
inline constexpr qint64 ZoneMaximumSize = 4LL * 1024 * 1024;
inline constexpr qint64 MeanMaxAggregateMaximumSize = 128LL * 1024 * 1024;
static_assert(
    ActivityMaximumSize + CacheMaximumSize
        <= MeanMaxAggregateMaximumSize,
    "The individual mean-max input budget must remain bounded");

qint64 maximumSize(FileKind kind);

enum class Status {
    Ready,
    Unavailable,
    InternalError
};

enum class Endpoint {
    RideDatabase,
    Activity,
    Zone,
    MeanMax
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

Contract contract(
    Endpoint endpoint,
    Status status,
    const QByteArray &series = QByteArray());

QString decodeRideDatabase(const QByteArray &contents);

} // namespace LocalApiEndpointInput

#endif
