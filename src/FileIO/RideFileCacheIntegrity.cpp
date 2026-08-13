/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideFileCacheIntegrity.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace {

constexpr qint64 MaximumCacheBytes = 256LL * 1024 * 1024;
constexpr int FixedZoneSizes[RideFileCacheIntegrity::ZoneBlockCount] = {
    10, 4, 10, 4, 10, 4, 4
};

static_assert(sizeof(float) == 4,
              "The CPX cache format requires 32-bit floats");
static_assert(sizeof(double) == 8,
              "The CPX cache format requires 64-bit doubles");
static_assert(sizeof(int) == 4,
              "The CPX cache format requires 32-bit ints");
static_assert(sizeof(unsigned int) == 4,
              "The CPX cache format requires 32-bit unsigned ints");
static_assert(sizeof(qint64) == 8,
              "The CPX cache format requires 64-bit qint64");
static_assert(std::numeric_limits<float>::is_iec559
                  && std::numeric_limits<double>::is_iec559,
              "The CPX cache format requires IEEE-754 floats");
static_assert(
    std::is_standard_layout_v<RideFileCacheHeader>,
    "The CPX cache header must have a stable native layout");
static_assert(
    std::is_trivially_copyable_v<RideFileCacheHeader>,
    "The CPX cache header must be copied as raw bytes");
static_assert(sizeof(RideFileCacheHeader) == 184,
              "Unexpected CPX cache header size");

#define GC_CPX_ASSERT_OFFSET(member, expected) \
    static_assert( \
        offsetof(RideFileCacheHeader, member) == expected, \
        "Unexpected CPX cache header offset")
GC_CPX_ASSERT_OFFSET(version, 0);
GC_CPX_ASSERT_OFFSET(crc, 4);
GC_CPX_ASSERT_OFFSET(wattsMeanMaxCount, 8);
GC_CPX_ASSERT_OFFSET(hrMeanMaxCount, 12);
GC_CPX_ASSERT_OFFSET(cadMeanMaxCount, 16);
GC_CPX_ASSERT_OFFSET(nmMeanMaxCount, 20);
GC_CPX_ASSERT_OFFSET(kphMeanMaxCount, 24);
GC_CPX_ASSERT_OFFSET(kphdMeanMaxCount, 28);
GC_CPX_ASSERT_OFFSET(wattsdMeanMaxCount, 32);
GC_CPX_ASSERT_OFFSET(caddMeanMaxCount, 36);
GC_CPX_ASSERT_OFFSET(nmdMeanMaxCount, 40);
GC_CPX_ASSERT_OFFSET(hrdMeanMaxCount, 44);
GC_CPX_ASSERT_OFFSET(xPowerMeanMaxCount, 48);
GC_CPX_ASSERT_OFFSET(npMeanMaxCount, 52);
GC_CPX_ASSERT_OFFSET(vamMeanMaxCount, 56);
GC_CPX_ASSERT_OFFSET(wattsKgMeanMaxCount, 60);
GC_CPX_ASSERT_OFFSET(aPowerMeanMaxCount, 64);
GC_CPX_ASSERT_OFFSET(aPowerKgMeanMaxCount, 68);
GC_CPX_ASSERT_OFFSET(wattsDistCount, 72);
GC_CPX_ASSERT_OFFSET(hrDistCount, 76);
GC_CPX_ASSERT_OFFSET(cadDistCount, 80);
GC_CPX_ASSERT_OFFSET(gearDistCount, 84);
GC_CPX_ASSERT_OFFSET(nmDistrCount, 88);
GC_CPX_ASSERT_OFFSET(kphDistCount, 92);
GC_CPX_ASSERT_OFFSET(xPowerDistCount, 96);
GC_CPX_ASSERT_OFFSET(npDistCount, 100);
GC_CPX_ASSERT_OFFSET(wattsKgDistCount, 104);
GC_CPX_ASSERT_OFFSET(aPowerDistCount, 108);
GC_CPX_ASSERT_OFFSET(smo2DistCount, 112);
GC_CPX_ASSERT_OFFSET(wbalDistCount, 116);
GC_CPX_ASSERT_OFFSET(LTHR, 120);
GC_CPX_ASSERT_OFFSET(CP, 124);
GC_CPX_ASSERT_OFFSET(CV, 128);
GC_CPX_ASSERT_OFFSET(WEIGHT, 136);
GC_CPX_ASSERT_OFFSET(WPRIME, 144);
GC_CPX_ASSERT_OFFSET(analysisSha256, 152);
#undef GC_CPX_ASSERT_OFFSET

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
    size = RideFileCacheIntegrity::CachePreambleBytes
        + RideFileCacheIntegrity::CacheFooterBytes;
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
        if (count <= 0
            || count > size - offset) {
            return false;
        }
        offset += count;
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
    sourceFingerprint =
        RideFileCRC::ContentFingerprint {};
    analysisFingerprint.clear();
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

bool validateCacheLayout(
    const RideFileCacheHeader &header,
    QString *error)
{
    if (error)
        error->clear();
    if (header.version
        != RideFileCacheVersion) {
        setError(
            error,
            QStringLiteral(
                "Unsupported CPX cache version"));
        return false;
    }
    if (header.crc
        > std::numeric_limits<quint16>::max()) {
        setError(
            error,
            QStringLiteral(
                "CPX cache legacy checksum is invalid"));
        return false;
    }
    qint64 expectedSize = 0;
    if (!expectedCacheSize(
            header, expectedSize)) {
        setError(
            error,
            QStringLiteral(
                "Invalid CPX cache block size"));
        return false;
    }
    return true;
}

bool setAnalysisFingerprint(
    RideFileCacheHeader &header,
    const QByteArray &fingerprint)
{
    if (fingerprint.size()
        != RideFileCRC::Sha256Size) {
        return false;
    }
    std::memcpy(
        header.analysisSha256,
        fingerprint.constData(),
        RideFileCRC::Sha256Size);
    return true;
}

QByteArray analysisFingerprint(
    const RideFileCacheHeader &header)
{
    return QByteArray(
        reinterpret_cast<const char *>(
            header.analysisSha256),
        RideFileCRC::Sha256Size);
}

bool inspectCache(QIODevice &input,
                  RideFileCacheHeader &header,
                  QString *error)
{
    RideFileCRC::ContentFingerprint sourceFingerprint;
    return inspectCache(
        input, header, sourceFingerprint, error);
}

bool inspectCache(
    QIODevice &input,
    RideFileCacheHeader &header,
    RideFileCRC::ContentFingerprint &sourceFingerprint,
    QString *error)
{
    header = RideFileCacheHeader {};
    sourceFingerprint =
        RideFileCRC::ContentFingerprint {};
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
    if (actualSize < CachePreambleBytes) {
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
    if (!validateCacheLayout(
            inspected, error)) {
        return false;
    }

    qint64 sourceByteSize = -1;
    QByteArray sourceSha256(
        RideFileCRC::Sha256Size,
        Qt::Uninitialized);
    if (!readExactly(
            input,
            reinterpret_cast<char *>(
                &sourceByteSize),
            sizeof(sourceByteSize))
        || !readExactly(
            input,
            sourceSha256.data(),
            sourceSha256.size())) {
        setError(
            error,
            QStringLiteral(
                "CPX cache source fingerprint is truncated"));
        return false;
    }

    RideFileCRC::ContentFingerprint inspectedSource;
    inspectedSource.byteSize = sourceByteSize;
    inspectedSource.sha256 =
        std::move(sourceSha256);
    inspectedSource.legacyCrc16 =
        static_cast<quint16>(inspected.crc);
    if (!inspectedSource.isValid()) {
        setError(
            error,
            QStringLiteral(
                "CPX cache source fingerprint is invalid"));
        return false;
    }

    qint64 expectedSize = 0;
    if (!expectedCacheSize(
            inspected, expectedSize)) {
        setError(
            error,
            QStringLiteral(
                "Invalid CPX cache block size"));
        return false;
    }
    if (actualSize != expectedSize) {
        setError(error, QStringLiteral("CPX cache payload size is invalid"));
        return false;
    }

    header = inspected;
    sourceFingerprint =
        std::move(inspectedSource);
    return true;
}

PartialReader::PartialReader(
    QIODevice &input,
    QString *error)
    : input_(&input)
{
    if (!inspectCache(
            input,
            header_,
            sourceFingerprint_,
            error)) {
        return;
    }
    counts_ = blockCounts(header_);
    analysisFingerprint_ =
        RideFileCacheIntegrity::analysisFingerprint(
            header_);
    if (!expectedCacheSize(
            header_, expectedSize_)) {
        setError(
            error,
            QStringLiteral(
                "Invalid CPX cache block size"));
        return;
    }
    try {
        preamble_.reserve(
            static_cast<qsizetype>(
                CachePreambleBytes));
        preamble_.append(
            reinterpret_cast<const char *>(
                &header_),
            sizeof(header_));
        const qint64 sourceByteSize =
            sourceFingerprint_.byteSize;
        preamble_.append(
            reinterpret_cast<const char *>(
                &sourceByteSize),
            sizeof(sourceByteSize));
        preamble_.append(
            sourceFingerprint_.sha256);
    } catch (const std::bad_alloc &) {
        valid_ = false;
        setError(
            error,
            QStringLiteral(
                "Cannot retain CPX cache preamble"));
        return;
    }
    if (preamble_.size()
        != CachePreambleBytes) {
        setError(
            error,
            QStringLiteral(
                "Invalid CPX cache preamble"));
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

const RideFileCRC::ContentFingerprint &
PartialReader::sourceFingerprint() const
{
    return sourceFingerprint_;
}

const QByteArray &
PartialReader::analysisFingerprint() const
{
    return analysisFingerprint_;
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
        || finished_
        || !blockLocation(
            counts_, block, firstFloat, count)) {
        setError(
            error,
            QStringLiteral(
                "Invalid CPX cache block"));
        return false;
    }
    return queueRead(
        firstFloat,
        count,
        &output,
        nullptr,
        error);
}

bool PartialReader::readZoneBlock(
    ZoneBlock block,
    QVector<float> &output,
    QString *error)
{
    output.clear();
    if (error)
        error->clear();
    qint64 firstFloat = 0;
    qint64 count = 0;
    if (!valid_
        || finished_
        || !zoneLocation(
            counts_, block, firstFloat, count)) {
        setError(
            error,
            QStringLiteral(
                "Invalid CPX cache zone block"));
        return false;
    }
    return queueRead(
        firstFloat,
        count,
        &output,
        nullptr,
        error);
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
        || finished_
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
    return queueRead(
        firstFloat + index,
        1,
        nullptr,
        &output,
        error);
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
        || finished_
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
    return queueRead(
        firstFloat + index,
        1,
        nullptr,
        &output,
        error);
}

bool PartialReader::queueRead(
    qint64 firstFloat,
    qint64 count,
    QVector<float> *vectorOutput,
    float *scalarOutput,
    QString *error)
{
    const qint64 byteOffset =
        CachePreambleBytes
        + firstFloat
            * static_cast<qint64>(
                sizeof(float));
    const qint64 byteCount =
        count
        * static_cast<qint64>(
            sizeof(float));
    if (!valid_
        || finished_
        || firstFloat < 0
        || count < 0
        || (vectorOutput == nullptr)
            == (scalarOutput == nullptr)
        || (scalarOutput && count != 1)
        || byteOffset < CachePreambleBytes
        || byteCount
            > expectedSize_
                - CacheFooterBytes
                - byteOffset) {
        setError(
            error,
            QStringLiteral(
                "Invalid CPX cache read request"));
        return false;
    }

    PendingRead pending;
    pending.firstFloat = firstFloat;
    pending.count = count;
    pending.vectorOutput = vectorOutput;
    pending.scalarOutput = scalarOutput;
    try {
        pendingReads_.append(
            std::move(pending));
    } catch (const std::bad_alloc &) {
        valid_ = false;
        setError(
            error,
            QStringLiteral(
                "Cannot queue CPX cache read"));
        return false;
    }
    return true;
}

void PartialReader::clearPendingOutputs()
{
    for (PendingRead &pending :
         pendingReads_) {
        if (pending.vectorOutput)
            pending.vectorOutput->clear();
        if (pending.scalarOutput)
            *pending.scalarOutput = 0.0f;
    }
}

bool PartialReader::finish(QString *error)
{
    if (error)
        error->clear();
    if (!valid_) {
        setError(
            error,
            QStringLiteral(
                "Invalid CPX cache reader"));
        return false;
    }
    if (finished_)
        return true;

    const auto fail = [this]() {
        clearPendingOutputs();
        valid_ = false;
        return false;
    };
    if (!input_
        || input_->size() != expectedSize_
        || !input_->seek(0)) {
        setError(
            error,
            QStringLiteral(
                "CPX cache changed before authenticated reading"));
        return fail();
    }

    try {
        for (PendingRead &pending :
             pendingReads_) {
            pending.staged.resize(
                static_cast<qsizetype>(
                    pending.count));
        }
    } catch (const std::bad_alloc &) {
        setError(
            error,
            QStringLiteral(
                "Cannot allocate staged CPX cache outputs"));
        return fail();
    }

    QCryptographicHash hash(
        QCryptographicHash::Sha256);
    QByteArray buffer(
        static_cast<int>(
            RideFileCRC::ReadChunkSize),
        Qt::Uninitialized);
    const qint64 protectedBytes =
        expectedSize_ - CacheFooterBytes;
    qint64 streamOffset = 0;
    while (streamOffset
           < protectedBytes) {
        const qint64 requested =
            std::min(
                protectedBytes
                    - streamOffset,
                static_cast<qint64>(
                    buffer.size()));
        const qint64 received =
            input_->read(
                buffer.data(),
                requested);
        if (received <= 0
            || received > requested) {
            setError(
                error,
                QStringLiteral(
                    "CPX cache authenticated stream is truncated"));
            return fail();
        }

        hash.addData(
            QByteArrayView(
                buffer.constData(),
                received));
        if (streamOffset
            < preamble_.size()) {
            const qint64 compared =
                std::min(
                    received,
                    static_cast<qint64>(
                        preamble_.size())
                        - streamOffset);
            if (std::memcmp(
                    buffer.constData(),
                    preamble_.constData()
                        + streamOffset,
                    static_cast<size_t>(
                        compared))
                != 0) {
                setError(
                    error,
                    QStringLiteral(
                        "CPX cache preamble changed while reading"));
                return fail();
            }
        }

        const qint64 streamEnd =
            streamOffset + received;
        for (PendingRead &pending :
             pendingReads_) {
            const qint64 pendingStart =
                CachePreambleBytes
                + pending.firstFloat
                    * static_cast<qint64>(
                        sizeof(float));
            const qint64 pendingEnd =
                pendingStart
                + pending.count
                    * static_cast<qint64>(
                        sizeof(float));
            const qint64 overlapStart =
                std::max(
                    streamOffset,
                    pendingStart);
            const qint64 overlapEnd =
                std::min(
                    streamEnd,
                    pendingEnd);
            if (overlapStart
                >= overlapEnd) {
                continue;
            }
            std::memcpy(
                reinterpret_cast<char *>(
                    pending.staged.data())
                    + overlapStart
                    - pendingStart,
                buffer.constData()
                    + overlapStart
                    - streamOffset,
                static_cast<size_t>(
                    overlapEnd
                    - overlapStart));
        }
        streamOffset = streamEnd;
    }

    QByteArray storedDigest(
        RideFileCRC::Sha256Size,
        Qt::Uninitialized);
    const QByteArray digest =
        hash.result();
    if (!readExactly(
            *input_,
            storedDigest.data(),
            storedDigest.size())
        || input_->pos() != expectedSize_
        || input_->size() != expectedSize_
        || storedDigest != digest) {
        setError(
            error,
            QStringLiteral(
                "CPX cache digest does not match its authenticated stream"));
        return fail();
    }

    for (PendingRead &pending :
         pendingReads_) {
        if (pending.vectorOutput) {
            *pending.vectorOutput =
                std::move(
                    pending.staged);
        } else {
            *pending.scalarOutput =
                pending.staged.constFirst();
        }
    }
    pendingReads_.clear();
    finished_ = true;
    return true;
}

bool readCache(QIODevice &input, CacheData &output, QString *error)
{
    output.clear();
    if (error)
        error->clear();

    PartialReader reader(
        input, error);
    if (!reader.isValid())
        return false;

    CacheData loaded;
    loaded.header =
        reader.header();
    loaded.sourceFingerprint =
        reader.sourceFingerprint();
    loaded.analysisFingerprint =
        reader.analysisFingerprint();
    for (int index = 0;
         index < BlockCount;
         ++index) {
        if (!reader.readBlock(
                static_cast<Block>(
                    index),
                loaded.blocks[index],
                error)) {
            return false;
        }
    }
    for (int index = 0;
         index < ZoneBlockCount;
         ++index) {
        if (!reader.readZoneBlock(
                static_cast<ZoneBlock>(
                    index),
                loaded.zones[index],
                error)) {
            return false;
        }
    }
    if (!reader.finish(error))
        return false;

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
    QVector<float> loaded;
    if (!reader.isValid()
        || !reader.readBlock(
            block, loaded, error)
        || !reader.finish(error)) {
        return false;
    }
    output = std::move(loaded);
    return true;
}

bool readBlockValue(QIODevice &input,
                    Block block,
                    qsizetype index,
                    float &output,
                    QString *error)
{
    output = 0.0f;
    PartialReader reader(input, error);
    float loaded = 0.0f;
    if (!reader.isValid()
        || !reader.readBlockValue(
            block, index, loaded, error)
        || !reader.finish(error)) {
        return false;
    }
    output = loaded;
    return true;
}

bool readZoneValue(QIODevice &input,
                   ZoneBlock block,
                   qsizetype index,
                   float &output,
                   QString *error)
{
    output = 0.0f;
    PartialReader reader(input, error);
    float loaded = 0.0f;
    if (!reader.isValid()
        || !reader.readZoneValue(
            block, index, loaded, error)
        || !reader.finish(error)) {
        return false;
    }
    output = loaded;
    return true;
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
