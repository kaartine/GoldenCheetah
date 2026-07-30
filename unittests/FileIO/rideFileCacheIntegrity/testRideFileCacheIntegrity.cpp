/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideFileCacheIntegrity.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QTemporaryDir>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {

constexpr int FixedZoneFloatCount = 10 + 4 + 10 + 4 + 10 + 4 + 4;

QByteArray cacheBytes(const RideFileCacheHeader &header,
                      int payloadFloatCount = FixedZoneFloatCount)
{
    QByteArray bytes(reinterpret_cast<const char *>(&header),
                     sizeof(header));
    const qint64 sourceByteSize = 4;
    bytes.append(
        reinterpret_cast<const char *>(
            &sourceByteSize),
        sizeof(sourceByteSize));
    bytes.append(
        RideFileCRC::Sha256Size,
        static_cast<char>(0x5a));
    bytes.append(payloadFloatCount * static_cast<int>(sizeof(float)), '\0');
    bytes.append(
        QCryptographicHash::hash(
            bytes,
            QCryptographicHash::Sha256));
    return bytes;
}

void resealCache(QByteArray &bytes)
{
    QVERIFY(
        bytes.size()
        >= RideFileCacheIntegrity::
            CacheFooterBytes);
    bytes.chop(
        RideFileCacheIntegrity::
            CacheFooterBytes);
    bytes.append(
        QCryptographicHash::hash(
            bytes,
            QCryptographicHash::Sha256));
}

QByteArray legacyCacheBytes(
    const RideFileCacheHeader &header)
{
    QByteArray bytes(
        reinterpret_cast<const char *>(&header),
        sizeof(header));
    bytes.append(
        FixedZoneFloatCount
            * static_cast<int>(sizeof(float)),
        '\0');
    return bytes;
}

void setFloat(QByteArray &bytes, qint64 byteOffset, float value)
{
    QVERIFY(byteOffset >= 0);
    QVERIFY(byteOffset + static_cast<qint64>(sizeof(value)) <= bytes.size());
    std::memcpy(bytes.data() + byteOffset, &value, sizeof(value));
}

bool readBytes(const QByteArray &bytes,
               RideFileCacheIntegrity::CacheData &data,
               QString *error = nullptr)
{
    QBuffer input;
    input.setData(bytes);
    if (!input.open(QIODevice::ReadOnly))
        qFatal("Could not open test input");
    return RideFileCacheIntegrity::readCache(input, data, error);
}

bool inspectBytes(const QByteArray &bytes,
                  RideFileCacheHeader &header,
                  QString *error = nullptr)
{
    QBuffer input;
    input.setData(bytes);
    if (!input.open(QIODevice::ReadOnly))
        qFatal("Could not open test input");
    return RideFileCacheIntegrity::inspectCache(input, header, error);
}

void verifyEmpty(const RideFileCacheIntegrity::CacheData &data)
{
    QVERIFY(data.isEmpty());
    for (const QVector<float> &block : data.blocks)
        QVERIFY(block.isEmpty());
    for (const QVector<float> &zone : data.zones)
        QVERIFY(zone.isEmpty());
}

RideFileCacheHeader validHeader()
{
    RideFileCacheHeader header {};
    header.version = RideFileCacheVersion;
    return header;
}

class ShortReadDevice : public QIODevice
{
public:
    ShortReadDevice(QByteArray bytes, qint64 readableBytes)
        : bytes_(std::move(bytes)), readableBytes_(readableBytes)
    {
        open(QIODevice::ReadOnly);
    }

    qint64 size() const override
    {
        return bytes_.size();
    }

    bool seek(qint64 position) override
    {
        if (position < 0 || position > bytes_.size())
            return false;
        position_ = position;
        return QIODevice::seek(position);
    }

protected:
    qint64 readData(char *data, qint64 maximumSize) override
    {
        if (position_ >= readableBytes_)
            return -1;
        const qint64 available = std::min(
            readableBytes_ - position_,
            static_cast<qint64>(bytes_.size()) - position_);
        const qint64 count = std::min(maximumSize, available);
        if (count <= 0)
            return 0;
        std::memcpy(data, bytes_.constData() + position_,
                    static_cast<size_t>(count));
        position_ += count;
        return count;
    }

    qint64 writeData(const char *, qint64) override
    {
        return -1;
    }

private:
    QByteArray bytes_;
    qint64 readableBytes_;
    qint64 position_ = 0;
};

class GrowingDevice : public QIODevice
{
public:
    explicit GrowingDevice(QByteArray bytes)
        : bytes_(std::move(bytes))
    {
        open(QIODevice::ReadOnly);
    }

    qint64 size() const override
    {
        ++sizeCalls_;
        return bytes_.size() - (sizeCalls_ == 1 ? 1 : 0);
    }

    bool seek(qint64 position) override
    {
        if (position < 0 || position > bytes_.size())
            return false;
        position_ = position;
        return QIODevice::seek(position);
    }

protected:
    qint64 readData(char *data, qint64 maximumSize) override
    {
        const qint64 available =
            static_cast<qint64>(bytes_.size()) - position_;
        const qint64 count = std::min(maximumSize, available);
        if (count <= 0)
            return 0;
        std::memcpy(data, bytes_.constData() + position_,
                    static_cast<size_t>(count));
        position_ += count;
        return count;
    }

    qint64 writeData(const char *, qint64) override
    {
        return -1;
    }

private:
    QByteArray bytes_;
    mutable int sizeCalls_ = 0;
    qint64 position_ = 0;
};

class ReplacingOnSecondZeroSeekDevice : public QIODevice
{
public:
    ReplacingOnSecondZeroSeekDevice(
        QByteArray initial,
        QByteArray replacement)
        : initial_(std::move(initial))
        , replacement_(std::move(replacement))
    {
        Q_ASSERT(initial_.size() == replacement_.size());
        open(QIODevice::ReadOnly);
    }

    qint64 size() const override
    {
        return initial_.size();
    }

    bool seek(qint64 position) override
    {
        if (position < 0 || position > initial_.size())
            return false;
        if (position == 0 && ++zeroSeekCount_ == 2)
            replaced_ = true;
        position_ = position;
        return QIODevice::seek(position);
    }

protected:
    qint64 readData(char *data, qint64 maximumSize) override
    {
        const QByteArray &bytes =
            replaced_ ? replacement_ : initial_;
        const qint64 available =
            static_cast<qint64>(bytes.size()) - position_;
        const qint64 count =
            std::min(maximumSize, available);
        if (count <= 0)
            return 0;
        std::memcpy(
            data,
            bytes.constData() + position_,
            static_cast<size_t>(count));
        position_ += count;
        return count;
    }

    qint64 writeData(const char *, qint64) override
    {
        return -1;
    }

private:
    QByteArray initial_;
    QByteArray replacement_;
    qint64 position_ = 0;
    int zeroSeekCount_ = 0;
    bool replaced_ = false;
};

class SparseCountingDevice : public QIODevice
{
public:
    explicit SparseCountingDevice(
        qint64 declaredSize)
        : declaredSize_(declaredSize)
    {
        open(
            QIODevice::ReadOnly
            | QIODevice::Unbuffered);
    }

    qint64 size() const override
    {
        return declaredSize_;
    }

    bool seek(qint64 position) override
    {
        if (position < 0
            || position > declaredSize_) {
            return false;
        }
        position_ = position;
        return QIODevice::seek(position);
    }

    qint64 bytesRead() const
    {
        return bytesRead_;
    }

protected:
    qint64 readData(
        char *data,
        qint64 maximumSize) override
    {
        const qint64 count =
            std::min({
                maximumSize,
                declaredSize_ - position_,
                qint64(4096)
            });
        if (count <= 0)
            return 0;
        std::memset(
            data,
            0,
            static_cast<size_t>(count));
        position_ += count;
        bytesRead_ += count;
        return count;
    }

    qint64 writeData(
        const char *,
        qint64) override
    {
        return -1;
    }

private:
    qint64 declaredSize_;
    qint64 position_ = 0;
    qint64 bytesRead_ = 0;
};

class SeekCountingBuffer : public QBuffer
{
public:
    using QBuffer::QBuffer;

    bool seek(qint64 position) override
    {
        if (position == 0)
            ++zeroSeekCount_;
        return QBuffer::seek(position);
    }

    int zeroSeekCount() const
    {
        return zeroSeekCount_;
    }

private:
    int zeroSeekCount_ = 0;
};

} // namespace

class TestRideFileCacheIntegrity : public QObject
{
    Q_OBJECT

private slots:
    void acceptsValidMinimalFormat();
    void acceptsNonUniformBlockLayout();
    void rejectsLegacyV25Cache();
    void inspectionAcceptsValidLayout();
    void inspectionRejectsTruncatedPayload();
    void rejectsTruncatedHeader_data();
    void rejectsTruncatedHeader();
    void rejectsTruncatedPayload_data();
    void rejectsTruncatedPayload();
    void rejectsUnsignedCountAbuse_data();
    void rejectsUnsignedCountAbuse();
    void rejectsExtraPayload();
    void rejectsSameSizePayloadCorruption();
    void rejectsCorruptCacheDigest();
    void rejectsInvalidSourceByteSize();
    void rejectsOutOfRangeLegacyCrc();
    void validatesSerializableLayout();
    void rejectsLargeInvalidCacheAfterHeader();
    void rejectsReplacementBetweenPayloadAndDigest();
    void rejectsShortReadAfterExactSizeValidation();
    void rejectsGrowthAfterLayoutValidation();
    void clearsOutputAfterFailure();
    void readsValidatedBlockWithoutLoadingOtherBlocks();
    void readsValidatedBlockAndZoneValues();
    void partialReaderValidatesOnceForMultipleReads();
    void partialReadsRejectInvalidIndexAndLayout();
    void partialReaderCorruptDigestIsAtomic();
    void partialReaderShortReadIsAtomicAndSticky();
    void partialReaderGrowthIsAtomicAndSticky();
    void atomicWritePreservesExistingFileOnFailure();
    void atomicWriteReplacesExistingFileOnSuccess();
    void atomicWriteRejectsInvalidatedSourceBeforeCommit();
    void atomicWriteCommitsAfterPreCommitValidation();
    void atomicWriteSkipsValidationAfterSerializationFailure();
    void atomicWriteReportsDefaultPreCommitValidationError();
    void validatesActivitySourcePath();
    void separatesPlannedAndCompletedCachePaths();
    void matchesCanonicalSourceUnderSymlinkedRoot();
    void rejectsSourceOutsideActivityRoots();
};

void TestRideFileCacheIntegrity::acceptsValidMinimalFormat()
{
    RideFileCacheIntegrity::CacheData data;
    QString error;

    QVERIFY2(readBytes(cacheBytes(validHeader()), data, &error),
             qPrintable(error));
    QVERIFY(data.complete);
    for (const QVector<float> &block : data.blocks)
        QVERIFY(block.isEmpty());

    const std::array<int, RideFileCacheIntegrity::ZoneBlockCount>
        expectedZoneSizes = {10, 4, 10, 4, 10, 4, 4};
    for (int index = 0;
         index < RideFileCacheIntegrity::ZoneBlockCount;
         ++index) {
        QCOMPARE(data.zones[index].size(), expectedZoneSizes[index]);
    }
}

void TestRideFileCacheIntegrity::acceptsNonUniformBlockLayout()
{
    RideFileCacheHeader header = validHeader();
    header.wattsMeanMaxCount = 1;
    header.wattsKgMeanMaxCount = 2;
    header.xPowerDistCount = 3;
    header.npDistCount = 1;
    RideFileCacheIntegrity::CacheData data;
    QString error;

    QVERIFY2(readBytes(cacheBytes(header, FixedZoneFloatCount + 7),
                       data, &error),
             qPrintable(error));
    QVERIFY(data.complete);
    QCOMPARE(data.blocks[RideFileCacheIntegrity::WattsMeanMax].size(), 1);
    QCOMPARE(data.blocks[RideFileCacheIntegrity::WattsKgMeanMax].size(), 2);
    QCOMPARE(data.blocks[RideFileCacheIntegrity::XPowerDistribution].size(),
             3);
    QCOMPARE(data.blocks[RideFileCacheIntegrity::NpDistribution].size(), 1);
}

void TestRideFileCacheIntegrity::rejectsLegacyV25Cache()
{
    RideFileCacheHeader header {};
    header.version = 25;
    RideFileCacheIntegrity::CacheData data;

    QVERIFY(!readBytes(
        legacyCacheBytes(header), data));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void TestRideFileCacheIntegrity::inspectionAcceptsValidLayout()
{
    RideFileCacheHeader expected = validHeader();
    expected.crc = 0x5678U;
    expected.wattsMeanMaxCount = 2;
    RideFileCacheHeader inspected {};
    RideFileCRC::ContentFingerprint
        sourceFingerprint;
    QString error;

    QByteArray bytes =
        cacheBytes(
            expected,
            FixedZoneFloatCount + 2);
    QBuffer input(&bytes);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVERIFY2(
        RideFileCacheIntegrity::inspectCache(
            input,
            inspected,
            sourceFingerprint,
            &error),
        qPrintable(error));
    QCOMPARE(inspected.version, expected.version);
    QCOMPARE(inspected.crc, expected.crc);
    QCOMPARE(inspected.wattsMeanMaxCount, expected.wattsMeanMaxCount);
    QCOMPARE(sourceFingerprint.byteSize, qint64(4));
    QCOMPARE(
        sourceFingerprint.sha256,
        QByteArray(
            RideFileCRC::Sha256Size,
            static_cast<char>(0x5a)));
    QCOMPARE(
        sourceFingerprint.legacyCrc16,
        static_cast<quint16>(
            expected.crc));
}

void TestRideFileCacheIntegrity::inspectionRejectsTruncatedPayload()
{
    const RideFileCacheHeader expected = validHeader();
    RideFileCacheHeader inspected {};
    inspected.version = 99;

    QVERIFY(!inspectBytes(cacheBytes(expected).chopped(1), inspected));
    QCOMPARE(inspected.version, 0U);
}

void TestRideFileCacheIntegrity::rejectsTruncatedHeader_data()
{
    QTest::addColumn<int>("size");
    QTest::newRow("empty") << 0;
    QTest::newRow("one-byte") << 1;
    QTest::newRow("last-header-byte") <<
        static_cast<int>(
            RideFileCacheIntegrity::
                CachePreambleBytes)
            - 1;
}

void TestRideFileCacheIntegrity::rejectsTruncatedHeader()
{
    QFETCH(int, size);
    QByteArray bytes(size, '\0');
    RideFileCacheIntegrity::CacheData data;

    QVERIFY(!readBytes(bytes, data));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void TestRideFileCacheIntegrity::rejectsTruncatedPayload_data()
{
    QTest::addColumn<QByteArray>("bytes");

    const RideFileCacheHeader header = validHeader();
    const QByteArray complete = cacheBytes(header);
    QTest::newRow("missing-all") <<
        complete.left(
            static_cast<int>(
                RideFileCacheIntegrity::
                    CachePreambleBytes));
    QTest::newRow("missing-last-byte") << complete.chopped(1);

    RideFileCacheHeader oneBlock = header;
    oneBlock.wattsMeanMaxCount = 1;
    QTest::newRow("declared-block-missing") << cacheBytes(oneBlock);
}

void TestRideFileCacheIntegrity::rejectsTruncatedPayload()
{
    QFETCH(QByteArray, bytes);
    RideFileCacheIntegrity::CacheData data;

    QVERIFY(!readBytes(bytes, data));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void TestRideFileCacheIntegrity::rejectsUnsignedCountAbuse_data()
{
    QTest::addColumn<quint32>("count");
    QTest::newRow("negative-one-bit-pattern") <<
        static_cast<quint32>(-1);
    QTest::newRow("signed-boundary") << quint32(0x80000000U);
    QTest::newRow("maximum") <<
        std::numeric_limits<quint32>::max();
}

void TestRideFileCacheIntegrity::rejectsUnsignedCountAbuse()
{
    QFETCH(quint32, count);
    RideFileCacheHeader header = validHeader();
    header.wattsMeanMaxCount = count;
    RideFileCacheIntegrity::CacheData data;

    QVERIFY(!readBytes(cacheBytes(header), data));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void TestRideFileCacheIntegrity::rejectsExtraPayload()
{
    QByteArray bytes = cacheBytes(validHeader());
    bytes.append('\0');
    RideFileCacheIntegrity::CacheData data;

    QVERIFY(!readBytes(bytes, data));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void
TestRideFileCacheIntegrity::
rejectsSameSizePayloadCorruption()
{
    RideFileCacheHeader header = validHeader();
    header.wattsMeanMaxCount = 1;
    QByteArray bytes =
        cacheBytes(
            header,
            FixedZoneFloatCount + 1);
    setFloat(
        bytes,
        RideFileCacheIntegrity::CachePreambleBytes,
        250.0f);
    resealCache(bytes);

    RideFileCacheIntegrity::CacheData data;
    QVERIFY(readBytes(bytes, data));

    bytes[
        RideFileCacheIntegrity::
            CachePreambleBytes] ^= 0x01;
    QVERIFY(!readBytes(bytes, data));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void
TestRideFileCacheIntegrity::
rejectsCorruptCacheDigest()
{
    QByteArray bytes =
        cacheBytes(validHeader());
    bytes[bytes.size() - 1] ^= 0x01;
    RideFileCacheIntegrity::CacheData data;

    QVERIFY(!readBytes(bytes, data));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void
TestRideFileCacheIntegrity::
rejectsInvalidSourceByteSize()
{
    QByteArray bytes =
        cacheBytes(validHeader());
    const qint64 invalidSize = -1;
    std::memcpy(
        bytes.data()
            + sizeof(RideFileCacheHeader),
        &invalidSize,
        sizeof(invalidSize));
    resealCache(bytes);
    RideFileCacheIntegrity::CacheData data;

    QVERIFY(!readBytes(bytes, data));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void
TestRideFileCacheIntegrity::
rejectsOutOfRangeLegacyCrc()
{
    RideFileCacheHeader header =
        validHeader();
    header.crc = 0x10000U;
    RideFileCacheIntegrity::CacheData data;

    QVERIFY(!readBytes(
        cacheBytes(header), data));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void
TestRideFileCacheIntegrity::
validatesSerializableLayout()
{
    RideFileCacheHeader header =
        validHeader();
    QString error;
    QVERIFY2(
        RideFileCacheIntegrity::
            validateCacheLayout(
                header, &error),
        qPrintable(error));

    header.wattsMeanMaxCount =
        std::numeric_limits<quint32>::max();
    QVERIFY(
        !RideFileCacheIntegrity::
            validateCacheLayout(
                header, &error));
    QVERIFY(!error.isEmpty());
}

void
TestRideFileCacheIntegrity::
rejectsLargeInvalidCacheAfterHeader()
{
    SparseCountingDevice input(
        256LL * 1024 * 1024);
    RideFileCacheIntegrity::CacheData data;

    QVERIFY(
        !RideFileCacheIntegrity::readCache(
            input, data));
    QCOMPARE(
        input.bytesRead(),
        static_cast<qint64>(
            sizeof(RideFileCacheHeader)));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void
TestRideFileCacheIntegrity::
rejectsReplacementBetweenPayloadAndDigest()
{
    RideFileCacheHeader header =
        validHeader();
    header.wattsMeanMaxCount = 1;
    QByteArray replacement =
        cacheBytes(
            header,
            FixedZoneFloatCount + 1);
    setFloat(
        replacement,
        RideFileCacheIntegrity::
            CachePreambleBytes,
        222.0f);
    resealCache(replacement);

    QByteArray invalidInitial =
        replacement;
    setFloat(
        invalidInitial,
        RideFileCacheIntegrity::
            CachePreambleBytes,
        111.0f);

    ReplacingOnSecondZeroSeekDevice
        fullInput(
            invalidInitial,
            replacement);
    RideFileCacheIntegrity::CacheData data;
    const bool fullRead =
        RideFileCacheIntegrity::readCache(
            fullInput, data);
    QVERIFY(fullRead);
    QCOMPARE(
        data.blocks[
            RideFileCacheIntegrity::
                WattsMeanMax]
            .constFirst(),
        222.0f);

    ReplacingOnSecondZeroSeekDevice
        partialInput(
            invalidInitial,
            replacement);
    float value = 999.0f;
    const bool partialRead =
        RideFileCacheIntegrity::
            readBlockValue(
                partialInput,
                RideFileCacheIntegrity::
                    WattsMeanMax,
                0,
                value);
    QVERIFY(partialRead);
    QCOMPARE(value, 222.0f);
}

void TestRideFileCacheIntegrity::rejectsShortReadAfterExactSizeValidation()
{
    const QByteArray bytes = cacheBytes(validHeader());
    ShortReadDevice input(bytes, bytes.size() - 1);
    RideFileCacheIntegrity::CacheData data;

    QVERIFY(!RideFileCacheIntegrity::readCache(input, data));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void TestRideFileCacheIntegrity::rejectsGrowthAfterLayoutValidation()
{
    QByteArray bytes = cacheBytes(validHeader());
    bytes.append('\0');
    GrowingDevice input(bytes);
    RideFileCacheIntegrity::CacheData data;

    QVERIFY(!RideFileCacheIntegrity::readCache(input, data));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void TestRideFileCacheIntegrity::clearsOutputAfterFailure()
{
    RideFileCacheIntegrity::CacheData data;
    data.blocks[RideFileCacheIntegrity::WattsMeanMax].append(300.0f);
    data.zones[RideFileCacheIntegrity::WattsTimeInZone].append(60.0f);

    QVERIFY(!readBytes(QByteArray(1, '\0'), data));
    QVERIFY(!data.complete);
    verifyEmpty(data);
}

void TestRideFileCacheIntegrity::readsValidatedBlockWithoutLoadingOtherBlocks()
{
    RideFileCacheHeader header = validHeader();
    header.wattsMeanMaxCount = 2;
    header.wattsKgMeanMaxCount = 1;
    header.hrMeanMaxCount = 1;
    QByteArray bytes =
        cacheBytes(header, FixedZoneFloatCount + 4);
    const qint64 payload =
        RideFileCacheIntegrity::CachePreambleBytes;
    setFloat(bytes, payload, 250.0f);
    setFloat(bytes, payload + sizeof(float), 240.0f);
    setFloat(bytes, payload + 2 * sizeof(float), 321.0f);
    setFloat(bytes, payload + 3 * sizeof(float), 155.0f);
    resealCache(bytes);

    QBuffer input(&bytes);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVector<float> values;
    QString error;
    QVERIFY2(RideFileCacheIntegrity::readBlock(
                 input,
                 RideFileCacheIntegrity::WattsKgMeanMax,
                 values,
                 &error),
             qPrintable(error));
    QCOMPARE(values, QVector<float>({321.0f}));
}

void TestRideFileCacheIntegrity::readsValidatedBlockAndZoneValues()
{
    RideFileCacheHeader header = validHeader();
    header.wattsMeanMaxCount = 2;
    QByteArray bytes =
        cacheBytes(header, FixedZoneFloatCount + 2);
    const qint64 payload =
        RideFileCacheIntegrity::CachePreambleBytes;
    setFloat(bytes, payload + sizeof(float), 240.0f);
    const qint64 zones = payload + 2 * sizeof(float);
    const qint64 heartRateZoneThree =
        zones + (10 + 4 + 2) * static_cast<qint64>(sizeof(float));
    setFloat(bytes, heartRateZoneThree, 93.0f);
    resealCache(bytes);

    QBuffer input(&bytes);
    QVERIFY(input.open(QIODevice::ReadOnly));
    float value = 0.0f;
    QString error;
    QVERIFY2(RideFileCacheIntegrity::readBlockValue(
                 input,
                 RideFileCacheIntegrity::WattsMeanMax,
                 1,
                 value,
                 &error),
             qPrintable(error));
    QCOMPARE(value, 240.0f);

    QVERIFY2(RideFileCacheIntegrity::readZoneValue(
                 input,
                 RideFileCacheIntegrity::HrTimeInZone,
                 2,
                 value,
                 &error),
             qPrintable(error));
    QCOMPARE(value, 93.0f);
}

void
TestRideFileCacheIntegrity::partialReaderValidatesOnceForMultipleReads()
{
    RideFileCacheHeader header = validHeader();
    header.wattsMeanMaxCount = 2;
    header.hrMeanMaxCount = 1;
    QByteArray bytes =
        cacheBytes(header, FixedZoneFloatCount + 3);
    const qint64 payload =
        RideFileCacheIntegrity::CachePreambleBytes;
    setFloat(bytes, payload, 250.0f);
    setFloat(bytes, payload + sizeof(float), 240.0f);
    setFloat(bytes, payload + 2 * sizeof(float), 155.0f);
    resealCache(bytes);

    SeekCountingBuffer input(&bytes);
    QVERIFY(input.open(QIODevice::ReadOnly));
    const int initialZeroSeeks =
        input.zeroSeekCount();
    QString error;
    RideFileCacheIntegrity::PartialReader reader(
        input, &error);
    QVERIFY2(reader.isValid(), qPrintable(error));
    float value = 0.0f;

    QVERIFY2(reader.readBlockValue(
                 RideFileCacheIntegrity::WattsMeanMax,
                 1,
                 value,
                 &error),
             qPrintable(error));
    QCOMPARE(value, 0.0f);
    float heartRateValue = 0.0f;
    QVERIFY2(reader.readBlockValue(
                 RideFileCacheIntegrity::HrMeanMax,
                 0,
                 heartRateValue,
                 &error),
             qPrintable(error));
    QCOMPARE(heartRateValue, 0.0f);
    QVERIFY2(
        reader.finish(&error),
        qPrintable(error));
    QCOMPARE(value, 240.0f);
    QCOMPARE(heartRateValue, 155.0f);
    QCOMPARE(
        input.zeroSeekCount() - initialZeroSeeks,
        2);
}

void TestRideFileCacheIntegrity::partialReadsRejectInvalidIndexAndLayout()
{
    QByteArray valid = cacheBytes(validHeader());
    QBuffer validInput(&valid);
    QVERIFY(validInput.open(QIODevice::ReadOnly));
    float value = 12.0f;
    QVERIFY(!RideFileCacheIntegrity::readBlockValue(
        validInput,
        RideFileCacheIntegrity::WattsMeanMax,
        0,
        value));
    QCOMPARE(value, 0.0f);

    QByteArray truncated = valid.chopped(1);
    QBuffer truncatedInput(&truncated);
    QVERIFY(truncatedInput.open(QIODevice::ReadOnly));
    QVector<float> values({1.0f});
    QVERIFY(!RideFileCacheIntegrity::readBlock(
        truncatedInput,
        RideFileCacheIntegrity::WattsMeanMax,
        values));
    QVERIFY(values.isEmpty());
}

void
TestRideFileCacheIntegrity::
partialReaderCorruptDigestIsAtomic()
{
    RideFileCacheHeader header =
        validHeader();
    header.wattsMeanMaxCount = 1;
    QByteArray bytes =
        cacheBytes(
            header,
            FixedZoneFloatCount + 1);
    setFloat(
        bytes,
        RideFileCacheIntegrity::
            CachePreambleBytes,
        123.456f);
    resealCache(bytes);
    bytes[bytes.size() - 1] ^= 0x01;

    QBuffer input(&bytes);
    QVERIFY(input.open(
        QIODevice::ReadOnly));
    QString error;
    RideFileCacheIntegrity::PartialReader
        reader(input, &error);
    QVERIFY2(
        reader.isValid(),
        qPrintable(error));

    float value = 999.0f;
    QVERIFY(
        reader.readBlockValue(
            RideFileCacheIntegrity::
                WattsMeanMax,
            0,
            value,
            &error));
    QCOMPARE(value, 0.0f);
    QVERIFY(!reader.finish(&error));
    QCOMPARE(value, 0.0f);
    QVERIFY(!reader.isValid());
}

void
TestRideFileCacheIntegrity::partialReaderShortReadIsAtomicAndSticky()
{
    RideFileCacheHeader header = validHeader();
    header.wattsMeanMaxCount = 1;
    QByteArray bytes =
        cacheBytes(header, FixedZoneFloatCount + 1);
    setFloat(
        bytes,
        RideFileCacheIntegrity::CachePreambleBytes,
        123.456f);
    resealCache(bytes);
    ShortReadDevice input(
        bytes,
        RideFileCacheIntegrity::CachePreambleBytes
            + 2);
    QString error;
    RideFileCacheIntegrity::PartialReader reader(input, &error);
    QVERIFY2(reader.isValid(), qPrintable(error));

    float value = 123.0f;
    QVERIFY(reader.readBlockValue(
        RideFileCacheIntegrity::WattsMeanMax,
        0,
        value,
        &error));
    QCOMPARE(value, 0.0f);
    QVERIFY(!reader.finish(&error));
    QVERIFY(!reader.isValid());

    value = 456.0f;
    QVERIFY(!reader.readZoneValue(
        RideFileCacheIntegrity::WattsTimeInZone,
        0,
        value,
        &error));
    QCOMPARE(value, 0.0f);
}

void
TestRideFileCacheIntegrity::partialReaderGrowthIsAtomicAndSticky()
{
    RideFileCacheHeader header = validHeader();
    header.wattsMeanMaxCount = 1;
    QByteArray bytes =
        cacheBytes(header, FixedZoneFloatCount + 1);
    setFloat(
        bytes,
        RideFileCacheIntegrity::CachePreambleBytes,
        123.456f);
    resealCache(bytes);
    bytes.append('\0');
    GrowingDevice input(bytes);
    QString error;
    RideFileCacheIntegrity::PartialReader reader(input, &error);
    QVERIFY2(reader.isValid(), qPrintable(error));

    float value = 123.0f;
    QVERIFY(reader.readBlockValue(
        RideFileCacheIntegrity::WattsMeanMax,
        0,
        value,
        &error));
    QCOMPARE(value, 0.0f);
    QVERIFY(!reader.finish(&error));
    QVERIFY(!reader.isValid());
}

void TestRideFileCacheIntegrity::atomicWritePreservesExistingFileOnFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("ride.cpx"));
    {
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("existing"), qint64(8));
    }

    QString error;
    QVERIFY(!RideFileCacheIntegrity::writeCacheAtomically(
        path,
        [](QIODevice &output, QString *) {
            return output.write("partial") == qint64(7) && false;
        },
        &error));
    QVERIFY(!error.isEmpty());

    QFile persisted(path);
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    QCOMPARE(persisted.readAll(), QByteArray("existing"));
}

void TestRideFileCacheIntegrity::atomicWriteReplacesExistingFileOnSuccess()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("ride.cpx"));
    {
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("existing"), qint64(8));
    }

    QString error;
    QVERIFY2(RideFileCacheIntegrity::writeCacheAtomically(
                 path,
                 [](QIODevice &output, QString *) {
                     return output.write("replacement") == qint64(11);
                 },
                 &error),
             qPrintable(error));

    QFile persisted(path);
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    QCOMPARE(persisted.readAll(), QByteArray("replacement"));
}

void
TestRideFileCacheIntegrity::atomicWriteRejectsInvalidatedSourceBeforeCommit()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("ride.cpx"));
    {
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("existing"), qint64(8));
    }

    int sourceGeneration = 1;
    const int serializedSourceGeneration = sourceGeneration;
    bool serializationFinished = false;
    bool validatorCalled = false;
    bool destinationReadableDuringValidation = false;
    QByteArray destinationDuringValidation;
    QString error;

    QVERIFY(!RideFileCacheIntegrity::writeCacheAtomically(
        path,
        [&](QIODevice &output, QString *) {
            if (output.write("replacement") != qint64(11))
                return false;
            serializationFinished = true;
            ++sourceGeneration;
            return true;
        },
        [&](QString *validationError) {
            validatorCalled = true;
            QFile destination(path);
            destinationReadableDuringValidation =
                destination.open(QIODevice::ReadOnly);
            if (destinationReadableDuringValidation)
                destinationDuringValidation = destination.readAll();

            const bool sourceIsCurrent =
                sourceGeneration == serializedSourceGeneration;
            if (!sourceIsCurrent && validationError) {
                *validationError = QStringLiteral(
                    "Activity source changed before CPX cache commit");
            }
            return serializationFinished && sourceIsCurrent;
        },
        &error));

    QVERIFY(serializationFinished);
    QVERIFY(validatorCalled);
    QVERIFY(destinationReadableDuringValidation);
    QCOMPARE(destinationDuringValidation, QByteArray("existing"));
    QCOMPARE(
        error,
        QStringLiteral(
            "Activity source changed before CPX cache commit"));

    QFile persisted(path);
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    QCOMPARE(persisted.readAll(), QByteArray("existing"));
}

void
TestRideFileCacheIntegrity::atomicWriteCommitsAfterPreCommitValidation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("ride.cpx"));
    {
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("existing"), qint64(8));
    }

    bool serializationFinished = false;
    bool validatorCalled = false;
    QByteArray destinationDuringValidation;
    QString error;
    QVERIFY2(RideFileCacheIntegrity::writeCacheAtomically(
                 path,
                 [&](QIODevice &output, QString *) {
                     serializationFinished =
                         output.write("replacement") == qint64(11);
                     return serializationFinished;
                 },
                 [&](QString *) {
                     validatorCalled = true;
                     QFile destination(path);
                     if (!destination.open(QIODevice::ReadOnly))
                         return false;
                     destinationDuringValidation = destination.readAll();
                     return serializationFinished;
                 },
                 &error),
             qPrintable(error));

    QVERIFY(validatorCalled);
    QCOMPARE(destinationDuringValidation, QByteArray("existing"));
    QFile persisted(path);
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    QCOMPARE(persisted.readAll(), QByteArray("replacement"));
}

void
TestRideFileCacheIntegrity::atomicWriteSkipsValidationAfterSerializationFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("ride.cpx"));
    bool validatorCalled = false;
    QString error;

    QVERIFY(!RideFileCacheIntegrity::writeCacheAtomically(
        path,
        [](QIODevice &output, QString *) {
            return output.write("partial") == qint64(7) && false;
        },
        [&](QString *) {
            validatorCalled = true;
            return true;
        },
        &error));

    QVERIFY(!validatorCalled);
    QCOMPARE(error, QStringLiteral("Cannot serialize CPX cache"));
    QVERIFY(!QFile::exists(path));
}

void
TestRideFileCacheIntegrity::
atomicWriteReportsDefaultPreCommitValidationError()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("ride.cpx"));
    QString error;

    QVERIFY(!RideFileCacheIntegrity::writeCacheAtomically(
        path,
        [](QIODevice &output, QString *) {
            return output.write("complete") == qint64(8);
        },
        [](QString *) {
            return false;
        },
        &error));

    QCOMPARE(
        error,
        QStringLiteral(
            "CPX cache pre-commit validation failed"));
    QVERIFY(!QFile::exists(path));
}

void TestRideFileCacheIntegrity::validatesActivitySourcePath()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString planned =
        directory.filePath(QStringLiteral("planned"));
    QVERIFY(QDir().mkdir(planned));
    const QString fileName =
        QStringLiteral("2026_07_29_12_00_00.fit");

    QCOMPARE(
        RideFileCacheIntegrity::activitySourcePath(
            planned, fileName),
        QDir::cleanPath(
            QDir(planned).filePath(fileName)));
    QVERIFY(
        RideFileCacheIntegrity::activitySourcePath(
            planned,
            QStringLiteral("../private.fit"))
            .isEmpty());
    QVERIFY(
        RideFileCacheIntegrity::activitySourcePath(
            planned,
            directory.filePath(fileName))
            .isEmpty());
}

void TestRideFileCacheIntegrity::separatesPlannedAndCompletedCachePaths()
{
    const QString root = QDir::cleanPath(
        QDir::tempPath() + QStringLiteral("/athlete"));
    const QString cache = root + QStringLiteral("/cache");
    const QString completed = root + QStringLiteral("/activities");
    const QString planned = root + QStringLiteral("/planned");
    const QString basename =
        QStringLiteral("2026_07_30_12_00_00.json");

    const QString completedCache =
        RideFileCacheIntegrity::cachePathForActivity(
            cache, completed, planned, completed + '/' + basename);
    const QString plannedCache =
        RideFileCacheIntegrity::cachePathForActivity(
            cache, completed, planned, planned + '/' + basename);

    QCOMPARE(completedCache,
             cache + QStringLiteral("/2026_07_30_12_00_00.cpx"));
    QCOMPARE(plannedCache,
             cache + QStringLiteral("/planned/2026_07_30_12_00_00.cpx"));
    QVERIFY(completedCache != plannedCache);
}

void
TestRideFileCacheIntegrity::matchesCanonicalSourceUnderSymlinkedRoot()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory symlink setup is Unix-specific");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString completed =
        directory.filePath(QStringLiteral("activities"));
    const QString planned =
        directory.filePath(QStringLiteral("planned"));
    const QString cache =
        directory.filePath(QStringLiteral("cache"));
    QVERIFY(QDir().mkdir(completed));
    QVERIFY(QDir().mkdir(planned));
    QVERIFY(QDir().mkdir(cache));
    const QString linkedCompleted =
        directory.filePath(
            QStringLiteral("activities-link"));
    QVERIFY(::symlink(
                QFile::encodeName(completed).constData(),
                QFile::encodeName(linkedCompleted).constData())
            == 0);
    const QString source =
        QDir(completed).filePath(
            QStringLiteral("2026_07_29_12_00_00.fit"));
    QFile sourceFile(source);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    sourceFile.close();

    QCOMPARE(
        RideFileCacheIntegrity::cachePathForActivity(
            cache,
            linkedCompleted,
            planned,
            source),
        QDir(cache).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.cpx")));
#endif
}

void TestRideFileCacheIntegrity::rejectsSourceOutsideActivityRoots()
{
    const QString root = QDir::cleanPath(
        QDir::tempPath() + QStringLiteral("/athlete"));
    const QString path = RideFileCacheIntegrity::cachePathForActivity(
        root + QStringLiteral("/cache"),
        root + QStringLiteral("/activities"),
        root + QStringLiteral("/planned"),
        root + QStringLiteral("/imports/2026_07_30_12_00_00.json"));

    QVERIFY(path.isEmpty());
}

QTEST_APPLESS_MAIN(TestRideFileCacheIntegrity)

#include "testRideFileCacheIntegrity.moc"
