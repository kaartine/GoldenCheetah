/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_OPEN_DATA_EXPORT_H
#define GC_OPEN_DATA_EXPORT_H

#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>
#include <memory>

class QIODevice;
class QFile;

namespace OpenDataExport {

struct Entry
{
    QString name;
    QByteArray contents;
};

struct Request
{
    QString athleteId;
    QString cyclist;
    int rideCount = 0;
    int formatVersion = 0;
    QList<Entry> entries;
    QString archivePath;
    qint64 archiveSize = 0;
    QByteArray archiveSha256;
};

using CancellationCheck = std::function<bool()>;
using ProgressCallback =
    std::function<void(int step, int lastStep, const QString &message)>;

struct ArchiveResult
{
    QByteArray contents;
    QString error;
    bool cancelled = false;

    bool ok() const
    {
        return !cancelled && error.isEmpty() && !contents.isEmpty();
    }
};

ArchiveResult buildArchive(
    const Request &request,
    const CancellationCheck &cancelled = {});

class ArchiveWriter final
{
public:
    explicit ArchiveWriter(const QString &path);
    ~ArchiveWriter();

    ArchiveWriter(const ArchiveWriter &) = delete;
    ArchiveWriter &operator=(const ArchiveWriter &) = delete;

    bool addFile(
        const QString &name,
        QIODevice *source,
        QString &error);
    bool finish(QString &error);

private:
    struct Private;
    std::unique_ptr<Private> d_;
};

enum class ArchiveDescriptionResult
{
    InProgress,
    Complete,
    Invalid
};

class ArchiveDescriptionBuilder final
{
public:
    explicit ArchiveDescriptionBuilder(const QString &path);
    ~ArchiveDescriptionBuilder();

    ArchiveDescriptionBuilder(
        const ArchiveDescriptionBuilder &) = delete;
    ArchiveDescriptionBuilder &operator=(
        const ArchiveDescriptionBuilder &) = delete;

    ArchiveDescriptionResult processNext(QString &error);
    qint64 bytesProcessed() const;
    qint64 size() const;
    QByteArray sha256() const;

private:
    struct Private;
    std::unique_ptr<Private> d_;
};

bool describeArchive(
    const QString &path,
    qint64 &size,
    QByteArray &sha256,
    QString &error);
bool validateArchive(
    const Request &request,
    QString &error);

enum class ArchiveValidationResult
{
    Valid,
    Invalid,
    Cancelled
};

bool openValidatedArchive(
    const Request &request,
    QFile &archive,
    QString &error);
ArchiveValidationResult openValidatedArchive(
    const Request &request,
    QFile &archive,
    const CancellationCheck &cancelled,
    QString &error);

struct UploadResult
{
    enum class Status
    {
        Succeeded,
        Failed,
        Cancelled
    };

    Status status = Status::Failed;
    QString error;

    static UploadResult succeeded();
    static UploadResult failed(const QString &error);
    static UploadResult cancelled();
};

using UploadTask = std::function<UploadResult(
    const Request &request,
    const CancellationCheck &cancelled,
    const ProgressCallback &progress)>;

} // namespace OpenDataExport

#endif
