/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideFileCacheIntegrity.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace {

constexpr qint64 MaximumCacheBytes = 256LL * 1024 * 1024;
constexpr int FixedZoneSizes[RideFileCacheIntegrity::ZoneBlockCount] = {
    10, 4, 10, 4, 10, 4, 4
};

static_assert(sizeof(float) == 4,
              "The CPX cache format requires 32-bit floats");

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

std::array<quint32, RideFileCacheIntegrity::BlockCount>
blockCounts(const RideFileCacheHeader &header)
{
    return {{
        header.wattsMeanMaxCount,
        header.wattsKgMeanMaxCount,
        header.hrMeanMaxCount,
        header.cadMeanMaxCount,
        header.nmMeanMaxCount,
        header.kphMeanMaxCount,
        header.kphdMeanMaxCount,
        header.wattsdMeanMaxCount,
        header.caddMeanMaxCount,
        header.nmdMeanMaxCount,
        header.hrdMeanMaxCount,
        header.xPowerMeanMaxCount,
        header.npMeanMaxCount,
        header.vamMeanMaxCount,
        header.aPowerMeanMaxCount,
        header.aPowerKgMeanMaxCount,
        header.wattsDistCount,
        header.hrDistCount,
        header.cadDistCount,
        header.gearDistCount,
        header.nmDistrCount,
        header.kphDistCount,
        header.xPowerDistCount,
        header.npDistCount,
        header.wattsKgDistCount,
        header.aPowerDistCount,
        header.smo2DistCount,
        header.wbalDistCount
    }};
}

bool addFloatsToSize(qint64 count, qint64 &size)
{
    if (count < 0 ||
        count > (MaximumCacheBytes - size) /
                    static_cast<qint64>(sizeof(float))) {
        return false;
    }
    size += count * static_cast<qint64>(sizeof(float));
    return true;
}

bool expectedCacheSize(const RideFileCacheHeader &header, qint64 &size)
{
    size = sizeof(header);
    const std::array<quint32, RideFileCacheIntegrity::BlockCount> counts =
        blockCounts(header);
    for (const quint32 count : counts) {
        if (!addFloatsToSize(count, size))
            return false;
    }
    for (const int count : FixedZoneSizes) {
        if (!addFloatsToSize(count, size))
            return false;
    }
    return true;
}

bool readExactly(QIODevice &input, char *destination, qint64 size)
{
    qint64 offset = 0;
    while (offset < size) {
        const qint64 count =
            input.read(destination + offset, size - offset);
        if (count <= 0)
            return false;
        offset += count;
    }
    return true;
}

bool readFloatRange(QIODevice &input,
                    qint64 firstFloat,
                    qint64 count,
                    float *destination,
                    qint64 expectedSize,
                    QString *error)
{
    const qint64 byteOffset =
        static_cast<qint64>(sizeof(RideFileCacheHeader))
        + firstFloat * static_cast<qint64>(sizeof(float));
    const qint64 byteCount =
        count * static_cast<qint64>(sizeof(float));
    if (!input.seek(byteOffset)) {
        setError(error, QStringLiteral("Cannot seek within CPX cache"));
        return false;
    }
    if (!readExactly(input, reinterpret_cast<char *>(destination),
                     byteCount)) {
        setError(error, QStringLiteral("CPX cache block is truncated"));
        return false;
    }
    if (input.size() != expectedSize) {
        setError(error, QStringLiteral("CPX cache changed while reading"));
        return false;
    }
    return true;
}

bool blockLocation(const std::array<quint32,
                                    RideFileCacheIntegrity::BlockCount>
                       &counts,
                   RideFileCacheIntegrity::Block block,
                   qint64 &firstFloat,
                   qint64 &count)
{
    const int blockIndex = static_cast<int>(block);
    if (blockIndex < 0
        || blockIndex >= RideFileCacheIntegrity::BlockCount) {
        return false;
    }
    firstFloat = 0;
    for (int index = 0; index < blockIndex; ++index)
        firstFloat += counts[index];
    count = counts[blockIndex];
    return true;
}

bool zoneLocation(const std::array<quint32,
                                   RideFileCacheIntegrity::BlockCount>
                      &counts,
                  RideFileCacheIntegrity::ZoneBlock block,
                  qint64 &firstFloat,
                  qint64 &count)
{
    const int blockIndex = static_cast<int>(block);
    if (blockIndex < 0
        || blockIndex >= RideFileCacheIntegrity::ZoneBlockCount) {
        return false;
    }
    firstFloat = 0;
    for (const quint32 blockCount : counts)
        firstFloat += blockCount;
    for (int index = 0; index < blockIndex; ++index)
        firstFloat += FixedZoneSizes[index];
    count = FixedZoneSizes[blockIndex];
    return true;
}

QString normalizedAbsolutePath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical =
        info.canonicalFilePath();
    if (!canonical.isEmpty())
        return QDir::cleanPath(canonical);

    const QString canonicalParent =
        info.dir().canonicalPath();
    if (!canonicalParent.isEmpty()) {
        return QDir::cleanPath(
            QDir(canonicalParent).filePath(
                info.fileName()));
    }
    return QDir::cleanPath(info.absoluteFilePath());
}

QString normalizedAbsoluteDirectory(const QString &path)
{
    const QString canonical =
        QDir(path).canonicalPath();
    return QDir::cleanPath(
        canonical.isEmpty()
            ? QDir(path).absolutePath()
            : canonical);
}

} // namespace

namespace RideFileCacheIntegrity {

void CacheData::clear()
{
    complete = false;
    header = RideFileCacheHeader {};
    for (QVector<float> &block : blocks)
        block.clear();
    for (QVector<float> &zone : zones)
        zone.clear();
}

bool CacheData::isEmpty() const
{
    return std::all_of(
               blocks.cbegin(), blocks.cend(),
               [](const QVector<float> &block) {
                   return block.isEmpty();
               }) &&
           std::all_of(
               zones.cbegin(), zones.cend(),
               [](const QVector<float> &zone) {
                   return zone.isEmpty();
               });
}

bool inspectCache(QIODevice &input,
                  RideFileCacheHeader &header,
                  QString *error)
{
    header = RideFileCacheHeader {};
    if (error)
        error->clear();

    if (!input.isOpen() || !(input.openMode() & QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("CPX cache is not open for reading"));
        return false;
    }
    if (input.isSequential()) {
        setError(error, QStringLiteral("CPX cache must be seekable"));
        return false;
    }

    const qint64 actualSize = input.size();
    if (actualSize < static_cast<qint64>(sizeof(RideFileCacheHeader))) {
        setError(error, QStringLiteral("CPX cache header is truncated"));
        return false;
    }
    if (actualSize > MaximumCacheBytes) {
        setError(error, QStringLiteral("CPX cache exceeds the size limit"));
        return false;
    }
    if (!input.seek(0)) {
        setError(error, QStringLiteral("Cannot seek to the CPX cache header"));
        return false;
    }

    RideFileCacheHeader inspected {};
    if (!readExactly(input, reinterpret_cast<char *>(&inspected),
                     sizeof(inspected))) {
        setError(error, QStringLiteral("CPX cache header is truncated"));
        return false;
    }
    if (inspected.version != RideFileCacheVersion) {
        setError(error, QStringLiteral("Unsupported CPX cache version"));
        return false;
    }

    qint64 expectedSize = 0;
    if (!expectedCacheSize(inspected, expectedSize)) {
        setError(error, QStringLiteral("Invalid CPX cache block size"));
        return false;
    }
    if (actualSize != expectedSize) {
        setError(error, QStringLiteral("CPX cache payload size is invalid"));
        return false;
    }

    header = inspected;
    return true;
}

PartialReader::PartialReader(
    QIODevice &input,
    QString *error)
    : input_(&input)
{
    if (!inspectCache(input, header_, error))
        return;
    counts_ = blockCounts(header_);
    if (!expectedCacheSize(
            header_, expectedSize_)) {
        setError(
            error,
            QStringLiteral(
                "Invalid CPX cache block size"));
        return;
    }
    valid_ = true;
}

bool PartialReader::isValid() const
{
    return valid_;
}

const RideFileCacheHeader &
PartialReader::header() const
{
    return header_;
}

bool PartialReader::readBlock(
    Block block,
    QVector<float> &output,
    QString *error)
{
    output.clear();
    if (error)
        error->clear();
    qint64 firstFloat = 0;
    qint64 count = 0;
    if (!valid_
        || !blockLocation(
            counts_, block, firstFloat, count)) {
        setError(
            error,
            QStringLiteral(
                "Invalid CPX cache block"));
        return false;
    }

    QVector<float> loaded;
    try {
        loaded.resize(
            static_cast<qsizetype>(count));
    } catch (const std::bad_alloc &) {
        setError(
            error,
            QStringLiteral(
                "Cannot allocate CPX cache block"));
        return false;
    }
    if (!readFloatRange(
            *input_,
            firstFloat,
            count,
            loaded.data(),
            expectedSize_,
            error)) {
        valid_ = false;
        return false;
    }
    output = std::move(loaded);
    return true;
}

bool PartialReader::readBlockValue(
    Block block,
    qsizetype index,
    float &output,
    QString *error)
{
    output = 0.0f;
    if (error)
        error->clear();
    qint64 firstFloat = 0;
    qint64 count = 0;
    if (!valid_
        || !blockLocation(
            counts_, block, firstFloat, count)
        || index < 0
        || index >= count) {
        setError(
            error,
            QStringLiteral(
                "Invalid CPX cache block index"));
        return false;
    }
    float loaded = 0.0f;
    if (!readFloatRange(
            *input_,
            firstFloat + index,
            1,
            &loaded,
            expectedSize_,
            error)) {
        valid_ = false;
        return false;
    }
    output = loaded;
    return true;
}

bool PartialReader::readZoneValue(
    ZoneBlock block,
    qsizetype index,
    float &output,
    QString *error)
{
    output = 0.0f;
    if (error)
        error->clear();
    qint64 firstFloat = 0;
    qint64 count = 0;
    if (!valid_
        || !zoneLocation(
            counts_, block, firstFloat, count)
        || index < 0
        || index >= count) {
        setError(
            error,
            QStringLiteral(
                "Invalid CPX cache zone index"));
        return false;
    }
    float loaded = 0.0f;
    if (!readFloatRange(
            *input_,
            firstFloat + index,
            1,
            &loaded,
            expectedSize_,
            error)) {
        valid_ = false;
        return false;
    }
    output = loaded;
    return true;
}

bool readCache(QIODevice &input, CacheData &output, QString *error)
{
    output.clear();
    if (error)
        error->clear();

    RideFileCacheHeader header {};
    if (!inspectCache(input, header, error))
        return false;

    const std::array<quint32, BlockCount> counts = blockCounts(header);
    qint64 expectedSize = 0;
    if (!expectedCacheSize(header, expectedSize)) {
        setError(error, QStringLiteral("Invalid CPX cache block size"));
        return false;
    }

    CacheData loaded;
    loaded.header = header;
    try {
        for (int index = 0; index < BlockCount; ++index)
            loaded.blocks[index].resize(static_cast<qsizetype>(counts[index]));
        for (int index = 0; index < ZoneBlockCount; ++index)
            loaded.zones[index].resize(FixedZoneSizes[index]);
    } catch (const std::bad_alloc &) {
        setError(error, QStringLiteral("Cannot allocate CPX cache arrays"));
        return false;
    }

    for (QVector<float> &block : loaded.blocks) {
        const qint64 bytes =
            static_cast<qint64>(block.size()) * sizeof(float);
        if (!readExactly(input, reinterpret_cast<char *>(block.data()),
                         bytes)) {
            setError(error, QStringLiteral("CPX cache block is truncated"));
            return false;
        }
    }
    for (QVector<float> &zone : loaded.zones) {
        const qint64 bytes =
            static_cast<qint64>(zone.size()) * sizeof(float);
        if (!readExactly(input, reinterpret_cast<char *>(zone.data()),
                         bytes)) {
            setError(error, QStringLiteral("CPX cache zone is truncated"));
            return false;
        }
    }
    if (input.pos() != expectedSize || input.size() != expectedSize) {
        setError(error, QStringLiteral("CPX cache payload was not read exactly"));
        return false;
    }

    loaded.complete = true;
    output = std::move(loaded);
    return true;
}

bool readBlock(QIODevice &input,
               Block block,
               QVector<float> &output,
               QString *error)
{
    output.clear();
    PartialReader reader(input, error);
    return reader.isValid()
        && reader.readBlock(
            block, output, error);
}

bool readBlockValue(QIODevice &input,
                    Block block,
                    qsizetype index,
                    float &output,
                    QString *error)
{
    output = 0.0f;
    PartialReader reader(input, error);
    return reader.isValid()
        && reader.readBlockValue(
            block, index, output, error);
}

bool readZoneValue(QIODevice &input,
                   ZoneBlock block,
                   qsizetype index,
                   float &output,
                   QString *error)
{
    output = 0.0f;
    PartialReader reader(input, error);
    return reader.isValid()
        && reader.readZoneValue(
            block, index, output, error);
}

bool writeCacheAtomically(const QString &path,
                          const CacheWriteOperation &write,
                          QString *error)
{
    return writeCacheAtomically(
        path, write, CachePreCommitValidator {}, error);
}

bool writeCacheAtomically(
    const QString &path,
    const CacheWriteOperation &write,
    const CachePreCommitValidator &validateBeforeCommit,
    QString *error)
{
    if (error)
        error->clear();
    if (path.isEmpty() || !write) {
        setError(error, QStringLiteral("Invalid CPX cache write request"));
        return false;
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        setError(error, output.errorString());
        return false;
    }

    QString writeError;
    if (!write(output, &writeError)) {
        output.cancelWriting();
        setError(error, writeError.isEmpty()
                            ? QStringLiteral("Cannot serialize CPX cache")
                            : writeError);
        return false;
    }
    if (output.error() != QFileDevice::NoError) {
        const QString deviceError = output.errorString();
        output.cancelWriting();
        setError(error, deviceError);
        return false;
    }
    if (!output.flush()) {
        const QString deviceError = output.errorString();
        output.cancelWriting();
        setError(error, deviceError.isEmpty()
                            ? QStringLiteral("Cannot flush CPX cache")
                            : deviceError);
        return false;
    }
    if (validateBeforeCommit) {
        QString validationError;
        if (!validateBeforeCommit(&validationError)) {
            output.cancelWriting();
            setError(
                error,
                validationError.isEmpty()
                    ? QStringLiteral(
                          "CPX cache pre-commit validation failed")
                    : validationError);
            return false;
        }
    }
    if (!output.commit()) {
        setError(error, output.errorString());
        return false;
    }
    return true;
}

QString activitySourcePath(
    const QString &activityDirectory,
    const QString &fileName)
{
    const QFileInfo directoryInfo(
        activityDirectory);
    const QFileInfo nameInfo(fileName);
    if (activityDirectory.isEmpty()
        || !directoryInfo.isAbsolute()
        || fileName.isEmpty()
        || fileName == QStringLiteral(".")
        || fileName == QStringLiteral("..")
        || nameInfo.isAbsolute()
        || nameInfo.fileName() != fileName) {
        return {};
    }
    return QDir::cleanPath(
        QDir(normalizedAbsoluteDirectory(
                 activityDirectory))
            .filePath(fileName));
}

QString cachePathForActivity(const QString &cacheRoot,
                             const QString &completedRoot,
                             const QString &plannedRoot,
                             const QString &sourceActivityPath)
{
    if (cacheRoot.isEmpty() || completedRoot.isEmpty() ||
        plannedRoot.isEmpty() || sourceActivityPath.isEmpty()) {
        return QString();
    }

    const QString completed =
        normalizedAbsoluteDirectory(completedRoot);
    const QString planned =
        normalizedAbsoluteDirectory(plannedRoot);
    if (completed == planned)
        return QString();

    const QFileInfo source(normalizedAbsolutePath(sourceActivityPath));
    const QString sourceDirectory =
        normalizedAbsoluteDirectory(source.absolutePath());
    QString relativeCacheDirectory;
    if (sourceDirectory == completed) {
        relativeCacheDirectory = QString();
    } else if (sourceDirectory == planned) {
        relativeCacheDirectory = QStringLiteral("planned");
    } else {
        return QString();
    }

    const QString basename = source.baseName();
    if (basename.isEmpty())
        return QString();

    QDir cache(normalizedAbsoluteDirectory(cacheRoot));
    if (!relativeCacheDirectory.isEmpty())
        cache = QDir(cache.filePath(relativeCacheDirectory));
    return QDir::cleanPath(cache.filePath(basename + QStringLiteral(".cpx")));
}

} // namespace RideFileCacheIntegrity
