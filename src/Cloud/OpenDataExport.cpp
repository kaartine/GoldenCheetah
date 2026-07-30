/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "OpenDataExport.h"

#include "../../contrib/qzip/zipwriter.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace {

constexpr qint64 HashChunkSize = 1024 * 1024;

bool cancellationRequested(
    const OpenDataExport::CancellationCheck &cancelled)
{
    if (!cancelled) return false;
    try {
        return cancelled();
    } catch (...) {
        return true;
    }
}

bool validEntryName(const QString &name)
{
    return !name.isEmpty()
        && name != QStringLiteral(".")
        && name != QStringLiteral("..")
        && !name.contains(QLatin1Char('/'))
        && !name.contains(QLatin1Char('\\'))
        && !name.contains(QChar::Null)
        && QFileInfo(name).fileName() == name;
}

QString zipError(ZipWriter::Status status)
{
    switch (status) {
    case ZipWriter::NoError:
        return {};
    case ZipWriter::FileWriteError:
        return QStringLiteral("Cannot write the OpenData archive");
    case ZipWriter::FileOpenError:
        return QStringLiteral("Cannot open the OpenData archive");
    case ZipWriter::FilePermissionsError:
        return QStringLiteral("Insufficient permissions for the OpenData archive");
    case ZipWriter::FileReadError:
        return QStringLiteral("Cannot read an OpenData archive entry");
    case ZipWriter::FileError:
        return QStringLiteral("Invalid OpenData archive device");
    }
    return QStringLiteral("Unknown OpenData archive error");
}

OpenDataExport::ArchiveValidationResult openAndDescribeArchive(
    const QString &path,
    QFile &archive,
    qint64 &size,
    QByteArray &sha256,
    const OpenDataExport::CancellationCheck &cancelled,
    QString &error)
{
    using Result = OpenDataExport::ArchiveValidationResult;
    size = 0;
    sha256.clear();
    error.clear();
    if (cancellationRequested(cancelled))
        return Result::Cancelled;

    const QFileInfo pathInfo(path);
    if (!pathInfo.isAbsolute()
        || !pathInfo.exists()
        || !pathInfo.isFile()
        || pathInfo.isSymLink()) {
        error = QStringLiteral("Invalid OpenData archive path");
        return Result::Invalid;
    }
    if (archive.isOpen()) {
        error = QStringLiteral(
            "OpenData archive device is already open");
        return Result::Invalid;
    }

    archive.setFileName(path);
    if (!archive.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read the OpenData archive: %1")
                    .arg(archive.errorString());
        return Result::Invalid;
    }
    const QFileInfo openedInfo(archive);
    size = archive.size();
    if (!openedInfo.isFile()
        || openedInfo.isSymLink()
        || size <= 0
        || quint64(size) > 0xfffffffeULL) {
        error = QStringLiteral("Invalid OpenData archive size or type");
        archive.close();
        return Result::Invalid;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!archive.atEnd()) {
        if (cancellationRequested(cancelled)) {
            archive.close();
            return Result::Cancelled;
        }
        const QByteArray chunk = archive.read(HashChunkSize);
        if (chunk.isEmpty() && !archive.atEnd()) {
            error = QStringLiteral(
                "Cannot hash the OpenData archive");
            archive.close();
            return Result::Invalid;
        }
        hash.addData(chunk);
    }
    if (archive.error() != QFileDevice::NoError
        || archive.pos() != size
        || !archive.seek(0)) {
        error = QStringLiteral(
            "Cannot hash the OpenData archive");
        archive.close();
        return Result::Invalid;
    }
    sha256 = hash.result();
    return Result::Valid;
}

} // namespace

namespace OpenDataExport {

struct ArchiveWriter::Private
{
    explicit Private(QString archivePath)
        : path(std::move(archivePath))
    {
    }

    QString path;
    QString initializationError;
    QSet<QString> names;
    std::unique_ptr<ZipWriter> writer;
    bool finished = false;
    bool writeFailed = false;
};

struct ArchiveDescriptionBuilder::Private
{
    explicit Private(QString archivePath)
        : path(std::move(archivePath))
        , hash(QCryptographicHash::Sha256)
    {
    }

    QString path;
    QFile archive;
    QCryptographicHash hash;
    qint64 expectedSize = 0;
    qint64 processed = 0;
    QByteArray digest;
    bool initialized = false;
    bool complete = false;
    bool invalid = false;
};

ArchiveWriter::ArchiveWriter(const QString &path)
    : d_(std::make_unique<Private>(path))
{
    const QFileInfo info(path);
    if (!info.isAbsolute()
        || (info.exists() && (!info.isFile() || info.isSymLink()))) {
        d_->initializationError =
            QStringLiteral("Invalid OpenData archive path");
        return;
    }

    if (!info.exists()) {
        QFile destination(path);
        if (!destination.open(
                QIODevice::WriteOnly | QIODevice::NewOnly)) {
            d_->initializationError =
                QStringLiteral("Cannot create the OpenData archive: %1")
                    .arg(destination.errorString());
            return;
        }
        destination.close();
    }
    if (!QFile::setPermissions(
            path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        d_->initializationError =
            QStringLiteral(
                "Cannot restrict OpenData archive permissions");
        return;
    }

    d_->writer = std::make_unique<ZipWriter>(path);
    d_->writer->setCompressionPolicy(ZipWriter::AutoCompress);
    if (d_->writer->status() != ZipWriter::NoError) {
        d_->initializationError =
            zipError(d_->writer->status());
        d_->writer.reset();
    }
}

ArchiveWriter::~ArchiveWriter() = default;

bool ArchiveWriter::addFile(
    const QString &name,
    QIODevice *source,
    QString &error)
{
    error.clear();
    if (!d_ || !d_->initializationError.isEmpty()) {
        error = d_
            ? d_->initializationError
            : QStringLiteral("Invalid OpenData archive writer");
        return false;
    }
    if (d_->finished || d_->writeFailed || !d_->writer) {
        error = QStringLiteral("OpenData archive is not writable");
        return false;
    }
    if (!validEntryName(name)) {
        error = QStringLiteral("Invalid OpenData archive entry name");
        return false;
    }
    if (d_->names.contains(name)) {
        error = QStringLiteral("Duplicate OpenData archive entry name");
        return false;
    }
    if (!source || !source->isReadable()) {
        error = QStringLiteral("Cannot read an OpenData archive entry");
        return false;
    }

    d_->writer->addFile(name, source);
    if (d_->writer->status() != ZipWriter::NoError) {
        d_->writeFailed = true;
        error = zipError(d_->writer->status());
        return false;
    }
    d_->names.insert(name);
    return true;
}

bool ArchiveWriter::finish(QString &error)
{
    error.clear();
    if (!d_ || !d_->initializationError.isEmpty()) {
        error = d_
            ? d_->initializationError
            : QStringLiteral("Invalid OpenData archive writer");
        return false;
    }
    if (d_->finished) {
        error = QStringLiteral("OpenData archive is already finished");
        return false;
    }
    d_->finished = true;
    if (d_->writeFailed || !d_->writer) {
        error = QStringLiteral("OpenData archive could not be written");
        return false;
    }
    if (d_->names.isEmpty()) {
        error = QStringLiteral("The OpenData export contains no entries");
        d_->writer.reset();
        return false;
    }

    d_->writer->close();
    const ZipWriter::Status status = d_->writer->status();
    d_->writer.reset();
    if (status != ZipWriter::NoError) {
        error = zipError(status);
        return false;
    }
    if (!QFile::setPermissions(
            d_->path,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        error = QStringLiteral(
            "Cannot restrict OpenData archive permissions");
        return false;
    }
    return true;
}

ArchiveDescriptionBuilder::ArchiveDescriptionBuilder(
    const QString &path)
    : d_(std::make_unique<Private>(path))
{
}

ArchiveDescriptionBuilder::~ArchiveDescriptionBuilder() = default;

ArchiveDescriptionResult
ArchiveDescriptionBuilder::processNext(QString &error)
{
    error.clear();
    if (!d_ || d_->invalid) {
        error = QStringLiteral(
            "Invalid OpenData archive description");
        return ArchiveDescriptionResult::Invalid;
    }
    if (d_->complete)
        return ArchiveDescriptionResult::Complete;

    if (!d_->initialized) {
        const QFileInfo pathInfo(d_->path);
        if (!pathInfo.isAbsolute()
            || !pathInfo.exists()
            || !pathInfo.isFile()
            || pathInfo.isSymLink()) {
            d_->invalid = true;
            error = QStringLiteral(
                "Invalid OpenData archive path");
            return ArchiveDescriptionResult::Invalid;
        }
        d_->archive.setFileName(d_->path);
        if (!d_->archive.open(QIODevice::ReadOnly)) {
            d_->invalid = true;
            error = QStringLiteral(
                "Cannot read the OpenData archive: %1")
                        .arg(d_->archive.errorString());
            return ArchiveDescriptionResult::Invalid;
        }
        const QFileInfo openedInfo(d_->archive);
        d_->expectedSize = d_->archive.size();
        if (!openedInfo.isFile()
            || openedInfo.isSymLink()
            || d_->expectedSize <= 0
            || quint64(d_->expectedSize) > 0xfffffffeULL) {
            d_->invalid = true;
            d_->archive.close();
            error = QStringLiteral(
                "Invalid OpenData archive size or type");
            return ArchiveDescriptionResult::Invalid;
        }
        d_->initialized = true;
    }

    const QByteArray chunk =
        d_->archive.read(HashChunkSize);
    if (chunk.isEmpty() && !d_->archive.atEnd()) {
        d_->invalid = true;
        d_->archive.close();
        error = QStringLiteral(
            "Cannot hash the OpenData archive");
        return ArchiveDescriptionResult::Invalid;
    }
    d_->hash.addData(chunk);
    d_->processed += chunk.size();
    if (!d_->archive.atEnd())
        return ArchiveDescriptionResult::InProgress;

    if (d_->archive.error() != QFileDevice::NoError
        || d_->processed != d_->expectedSize) {
        d_->invalid = true;
        d_->archive.close();
        error = QStringLiteral(
            "Cannot hash the OpenData archive");
        return ArchiveDescriptionResult::Invalid;
    }
    d_->digest = d_->hash.result();
    d_->archive.close();
    d_->complete = true;
    return ArchiveDescriptionResult::Complete;
}

qint64 ArchiveDescriptionBuilder::bytesProcessed() const
{
    return d_ ? d_->processed : 0;
}

qint64 ArchiveDescriptionBuilder::size() const
{
    return d_ && d_->complete
        ? d_->expectedSize
        : 0;
}

QByteArray ArchiveDescriptionBuilder::sha256() const
{
    return d_ && d_->complete
        ? d_->digest
        : QByteArray();
}

bool describeArchive(
    const QString &path,
    qint64 &size,
    QByteArray &sha256,
    QString &error)
{
    QFile archive(path);
    return openAndDescribeArchive(
               path, archive, size, sha256, {}, error)
        == ArchiveValidationResult::Valid;
}

bool validateArchive(
    const Request &request,
    QString &error)
{
    qint64 actualSize = 0;
    QByteArray actualSha256;
    if (!describeArchive(
            request.archivePath,
            actualSize,
            actualSha256,
            error)) {
        return false;
    }
    if (actualSize != request.archiveSize
        || actualSha256 != request.archiveSha256) {
        error = QStringLiteral(
            "The OpenData archive changed after capture");
        return false;
    }
    return true;
}

bool openValidatedArchive(
    const Request &request,
    QFile &archive,
    QString &error)
{
    return openValidatedArchive(
               request, archive, {}, error)
        == ArchiveValidationResult::Valid;
}

ArchiveValidationResult openValidatedArchive(
    const Request &request,
    QFile &archive,
    const CancellationCheck &cancelled,
    QString &error)
{
    qint64 actualSize = 0;
    QByteArray actualSha256;
    const ArchiveValidationResult validation =
        openAndDescribeArchive(
            request.archivePath,
            archive,
            actualSize,
            actualSha256,
            cancelled,
            error);
    if (validation != ArchiveValidationResult::Valid)
        return validation;
    if (actualSize != request.archiveSize
        || actualSha256 != request.archiveSha256) {
        error = QStringLiteral(
            "The OpenData archive changed after capture");
        archive.close();
        return ArchiveValidationResult::Invalid;
    }
    return ArchiveValidationResult::Valid;
}

ArchiveResult
buildArchive(
    const Request &request,
    const CancellationCheck &cancelled)
{
    ArchiveResult result;
    if (cancellationRequested(cancelled)) {
        result.cancelled = true;
        return result;
    }
    if (request.entries.isEmpty()) {
        result.error = QStringLiteral("The OpenData export contains no entries");
        return result;
    }

    QSet<QString> names;
    for (const Entry &entry : request.entries) {
        if (!validEntryName(entry.name)) {
            result.error =
                QStringLiteral("Invalid OpenData archive entry name");
            return result;
        }
        if (names.contains(entry.name)) {
            result.error =
                QStringLiteral("Duplicate OpenData archive entry name");
            return result;
        }
        names.insert(entry.name);
    }

    QByteArray archive;
    QBuffer destination(&archive);
    if (!destination.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("Cannot prepare the OpenData archive");
        return result;
    }

    ZipWriter writer(&destination);
    writer.setCompressionPolicy(ZipWriter::AutoCompress);
    if (writer.status() != ZipWriter::NoError) {
        result.error = zipError(writer.status());
        return result;
    }

    for (const Entry &entry : request.entries) {
        if (cancellationRequested(cancelled)) {
            writer.close();
            result.cancelled = true;
            return result;
        }
        writer.addFile(entry.name, entry.contents);
        if (writer.status() != ZipWriter::NoError) {
            result.error = zipError(writer.status());
            writer.close();
            return result;
        }
    }

    writer.close();
    if (writer.status() != ZipWriter::NoError) {
        result.error = zipError(writer.status());
        return result;
    }
    if (cancellationRequested(cancelled)) {
        result.cancelled = true;
        return result;
    }
    if (archive.isEmpty()) {
        result.error = QStringLiteral("The OpenData archive is empty");
        return result;
    }

    result.contents = std::move(archive);
    return result;
}

UploadResult UploadResult::succeeded()
{
    return {Status::Succeeded, {}};
}

UploadResult UploadResult::failed(const QString &error)
{
    return {Status::Failed, error};
}

UploadResult UploadResult::cancelled()
{
    return {Status::Cancelled, {}};
}

} // namespace OpenDataExport
