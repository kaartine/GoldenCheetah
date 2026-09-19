/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "LocalApiEndpointInput.h"

#include <QBuffer>
#include <QFileInfo>
#include <QTextStream>

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
    }
    return -1;
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

Contract contract(
    Endpoint endpoint,
    Status status,
    const QByteArray &series)
{
    Contract result;
    if (status == Status::Ready) {
        result.processInput = true;
        if (endpoint == Endpoint::MeanMax) {
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
