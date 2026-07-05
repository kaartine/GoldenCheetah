/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "TTSReader.h"

#include <QBuffer>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

using NS_TTSReader::Byte;
using NS_TTSReader::ByteArray;
using NS_TTSReader::TTSReader;
using NS_TTSReader::UByte;

namespace {

struct BlockSpec {
    int type;
    int version;
    const char *name;
};

constexpr BlockSpec blockSpecs[] = {
    { 5020, 1000, "distance-frame" },
    { 1031, 1000, "general-info" },
    { 5010, 1004, "training-type" },
    { 5050, 1000, "gps-data" },
    { 1032, 1000, "program-data" },
    { 1041, 1000, "segment-info-v1000" },
    { 1041, 1104, "segment-info-v1104" },
    { 1050, 1000, "segment-range" },
    { 2010, 1000, "video-info" },
};

Byte byte(unsigned value)
{
    return static_cast<Byte>(value);
}

void appendUInt16(ByteArray &data, std::uint16_t value)
{
    data.push_back(byte(value));
    data.push_back(byte(value >> 8));
}

void appendUInt32(ByteArray &data, std::uint32_t value)
{
    data.push_back(byte(value));
    data.push_back(byte(value >> 8));
    data.push_back(byte(value >> 16));
    data.push_back(byte(value >> 24));
}

void appendFloat(ByteArray &data, float value)
{
    std::uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value), "float must be IEEE-754 binary32");
    std::memcpy(&bits, &value, sizeof(bits));
    appendUInt32(data, bits);
}

// Low bytes of the complete Wattzap rehashed data key.
constexpr std::string_view ttsDataKey =
    "Kermit rules~@!! He is the sAvior of eVery 56987()$*(#& ) needed "
    "Pr0t3cTION SYSTEM.JustBESUREto%^m4kethis*$tringl____ng'nuff!";
static_assert(ttsDataKey.size() == 125);

ByteArray makeHeader(std::uint16_t headerId, std::uint16_t blockType,
                     std::uint16_t version, std::uint32_t elementSize,
                     std::uint32_t elementCount)
{
    Q_ASSERT(headerId <= 20);

    ByteArray header;
    appendUInt16(header, headerId);
    appendUInt16(header, blockType);
    appendUInt16(header, version);
    appendUInt32(header, elementSize);
    appendUInt32(header, elementCount);
    return header;
}

void appendEncryptedBlockWithDimensions(QByteArray &fixture,
                                        std::uint16_t headerId,
                                        std::uint16_t blockType,
                                        std::uint16_t version,
                                        std::uint32_t elementSize,
                                        std::uint32_t elementCount,
                                        const ByteArray &payload)
{
    const ByteArray header = makeHeader(headerId, blockType, version,
                                        elementSize, elementCount);

    fixture.append(header.data(), static_cast<int>(header.size()));

    QByteArray encrypted;
    encrypted.reserve(static_cast<int>(payload.size()));
    for (size_t i = 0; i < payload.size(); ++i) {
        const size_t keyIndex = i % ttsDataKey.size();
        const UByte keyByte =
            static_cast<UByte>(header[keyIndex % header.size()])
            ^ static_cast<UByte>(ttsDataKey[keyIndex]);
        encrypted.append(static_cast<char>(static_cast<UByte>(payload[i])
                                           ^ keyByte));
    }

    fixture.append(encrypted);
}

void appendEncryptedBlock(QByteArray &fixture, std::uint16_t headerId,
                          std::uint16_t blockType, std::uint16_t version,
                          const ByteArray &payload)
{
    appendEncryptedBlockWithDimensions(
        fixture, headerId, blockType, version,
        static_cast<std::uint32_t>(payload.size()), 1, payload);
}

void appendProgramRecord(ByteArray &data, std::int16_t slope,
                         std::uint32_t distance)
{
    appendUInt16(data, static_cast<std::uint16_t>(slope));
    appendUInt32(data, distance);
}

void appendGpsRecord(ByteArray &data, std::uint32_t distance,
                     float latitude, float longitude, float altitude)
{
    appendUInt32(data, distance);
    appendFloat(data, latitude);
    appendFloat(data, longitude);
    appendFloat(data, altitude);
}

void appendFrameRecord(ByteArray &data, std::uint32_t distance,
                       std::uint32_t frame)
{
    appendUInt32(data, distance);
    appendUInt32(data, frame);
}

QByteArray realisticFixture()
{
    QByteArray fixture;

    const ByteArray generalInfo = { byte(0), byte(0) };
    appendEncryptedBlock(fixture, 1, 1031, 1000, generalInfo);

    ByteArray program;
    appendProgramRecord(program, 100, 0);
    appendProgramRecord(program, 200, 10000);
    appendProgramRecord(program, -50, 10000);
    appendEncryptedBlock(fixture, 2, 1032, 1000, program);

    ByteArray gps;
    appendGpsRecord(gps, 0, 60.1699f, 24.9384f, 42.5f);
    appendGpsRecord(gps, 10000, 60.1704f, 24.9391f, 43.25f);
    appendGpsRecord(gps, 20000, 60.1710f, 24.9400f, 44.0f);
    appendEncryptedBlock(fixture, 3, 5050, 1000, gps);

    ByteArray frames;
    appendFrameRecord(frames, 0, 0);
    appendFrameRecord(frames, 10000, 30);
    appendFrameRecord(frames, 20000, 60);
    appendEncryptedBlock(fixture, 4, 5020, 1000, frames);

    ByteArray range;
    appendUInt32(range, 0);
    appendUInt32(range, 20000);
    appendUInt16(range, 0x1234);
    appendEncryptedBlock(fixture, 5, 1050, 1000, range);

    return fixture;
}

QByteArray malformedFixture()
{
    QByteArray fixture;
    const ByteArray malformedProgram(7, byte(0));
    appendEncryptedBlock(fixture, 1, 1032, 1000, malformedProgram);
    return fixture;
}

QByteArray lateMalformedFixture()
{
    QByteArray fixture = realisticFixture();
    const ByteArray malformedProgram(7, byte(0));
    appendEncryptedBlock(fixture, 6, 1032, 1000, malformedProgram);
    return fixture;
}

QByteArray headerLikeCiphertextFixture()
{
    QByteArray fixture;

    // With header id 1 these plaintext bytes encrypt to 0x0001, which is
    // indistinguishable from a header if payload boundaries are ignored.
    const ByteArray generalInfo = { byte(0x4b), byte(0x65) };
    appendEncryptedBlock(fixture, 1, 1031, 1000, generalInfo);

    ByteArray program;
    appendProgramRecord(program, 100, 0);
    appendProgramRecord(program, 200, 10000);
    appendEncryptedBlock(fixture, 2, 1032, 1000, program);
    return fixture;
}

bool parseFixture(TTSReader &reader, QByteArray fixture)
{
    QBuffer buffer(&fixture);
    if (!buffer.open(QIODevice::ReadOnly))
        return false;

    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);
    return reader.parseFile(stream);
}

class PrefixOnlySequentialDevice : public QIODevice
{
public:
    explicit PrefixOnlySequentialDevice(QByteArray header)
        : header_(std::move(header))
    {
    }

    bool isSequential() const override
    {
        return true;
    }

    int postPrefixReadRequests() const
    {
        return postPrefixReadRequests_;
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        if (offset_ >= header_.size()) {
            ++postPrefixReadRequests_;
            return -1;
        }

        const qint64 available = header_.size() - offset_;
        const qint64 bytes = std::min(maxSize, available);
        std::memcpy(data, header_.constData() + offset_,
                    static_cast<size_t>(bytes));
        offset_ += bytes;
        return bytes;
    }

    qint64 writeData(const char *, qint64) override
    {
        return -1;
    }

private:
    QByteArray header_;
    qint64 offset_ = 0;
    int postPrefixReadRequests_ = 0;
};

bool expectedToAccept(const BlockSpec &block, int size)
{
    switch (block.type) {
    case 1032:
        return size >= 6 && size % 6 == 0;
    case 5020:
        return size % 8 == 0;
    case 5050:
        return size % 16 == 0;
    case 1031:
        return size >= 2;
    case 5010:
        return block.version != 1004 || size >= 6;
    case 1041:
        return size == (block.version == 1000 ? 10 : 8);
    case 1050:
        return size % 10 == 0;
    default:
        return true;
    }
}

} // namespace

class TestTTSReaderBounds : public QObject
{
    Q_OBJECT

private slots:
    void emptyAndOneToFifteenByteBlocks_data()
    {
        QTest::addColumn<int>("blockType");
        QTest::addColumn<int>("version");
        QTest::addColumn<int>("size");
        QTest::addColumn<bool>("accepted");

        for (const BlockSpec &block : blockSpecs) {
            for (int size = 0; size <= 15; ++size) {
                const QByteArray row = QByteArray(block.name)
                    + '-' + QByteArray::number(size);
                QTest::newRow(row.constData())
                    << block.type << block.version << size
                    << expectedToAccept(block, size);
            }
        }
    }

    void emptyAndOneToFifteenByteBlocks()
    {
        QFETCH(int, blockType);
        QFETCH(int, version);
        QFETCH(int, size);
        QFETCH(bool, accepted);

        TTSReader reader;
        ByteArray data(static_cast<size_t>(size), byte(1));
        QCOMPARE(reader.blockProcessing(blockType, version, data), accepted);

        // Sanitizers provide the bounds assertion for this exhaustive matrix.
        QVERIFY(reader.getPoints().empty());
    }

    void emptyOptionalBlocksAreNoOps_data()
    {
        QTest::addColumn<int>("blockType");
        QTest::addColumn<int>("version");

        QTest::newRow("frame-mapping") << 5020 << 1000;
        QTest::newRow("gps") << 5050 << 1000;
        QTest::newRow("segment-range") << 1050 << 1000;
    }

    void emptyOptionalBlocksAreNoOps()
    {
        QFETCH(int, blockType);
        QFETCH(int, version);

        TTSReader reader;
        ByteArray empty;

        QVERIFY(reader.blockProcessing(blockType, version, empty));
        QCOMPARE(reader.getFrameRate(), 0.0);
        QVERIFY(!reader.hasFrameMapping());
        QVERIFY(reader.getPoints().empty());
        QVERIFY(reader.getSegmentRanges().empty());
    }

    void emptyRequiredBlocksAreRejected_data()
    {
        QTest::addColumn<int>("blockType");
        QTest::addColumn<int>("version");

        QTest::newRow("program") << 1032 << 1000;
        QTest::newRow("general-info") << 1031 << 1000;
        QTest::newRow("training-type") << 5010 << 1004;
        QTest::newRow("segment-info-v1000") << 1041 << 1000;
        QTest::newRow("segment-info-v1104") << 1041 << 1104;
    }

    void emptyRequiredBlocksAreRejected()
    {
        QFETCH(int, blockType);
        QFETCH(int, version);

        TTSReader reader;
        ByteArray empty;

        QVERIFY(!reader.blockProcessing(blockType, version, empty));
    }

    void rejectsZeroFrameRateWithoutMutatingState()
    {
        TTSReader reader;
        ByteArray data;
        appendFrameRecord(data, 0, 0);

        QVERIFY(!reader.blockProcessing(5020, 1000, data));
        QCOMPARE(reader.getFrameRate(), 0.0);
        QVERIFY(!reader.hasFrameMapping());
        QVERIFY(!reader.loadHeaders());
        QVERIFY(reader.getPoints().empty());
    }

    void nonIncreasingFrameTimesProduceFiniteZeroSpeed_data()
    {
        QTest::addColumn<std::uint32_t>("middleFrame");
        QTest::addColumn<std::uint32_t>("lastFrame");

        QTest::newRow("duplicate") << std::uint32_t(30) << std::uint32_t(30);
        QTest::newRow("decreasing") << std::uint32_t(30) << std::uint32_t(20);
    }

    void nonIncreasingFrameTimesProduceFiniteZeroSpeed()
    {
        QFETCH(std::uint32_t, middleFrame);
        QFETCH(std::uint32_t, lastFrame);

        TTSReader reader;
        ByteArray data;
        appendFrameRecord(data, 0, 0);
        appendFrameRecord(data, 10000, middleFrame);
        appendFrameRecord(data, 20000, lastFrame);

        QVERIFY(reader.blockProcessing(5020, 1000, data));
        QVERIFY(reader.loadHeaders());

        const std::vector<NS_TTSReader::Point> &points = reader.getPoints();
        QCOMPARE(points.size(), size_t(3));
        for (const NS_TTSReader::Point &point : points)
            QVERIFY(std::isfinite(point.getSpeed()));
        QCOMPARE(points.back().getSpeed(), 0.0);
    }

    void slopeOnlyRouteUsesFiniteZeroSpeedWithoutFrameTimes()
    {
        TTSReader reader;
        ByteArray data;
        appendProgramRecord(data, 100, 0);
        appendProgramRecord(data, 200, 10000);

        QVERIFY(reader.blockProcessing(1032, 1000, data));
        QVERIFY(reader.loadHeaders());

        const std::vector<NS_TTSReader::Point> &points = reader.getPoints();
        QCOMPARE(points.size(), size_t(2));
        for (const NS_TTSReader::Point &point : points) {
            QVERIFY(std::isfinite(point.getSpeed()));
            QCOMPARE(point.getSpeed(), 0.0);
        }
    }

    void decodesValidFrameMappingWithoutIntegerConversion()
    {
        TTSReader reader;
        ByteArray data;
        appendFrameRecord(data, 0, 0);
        appendFrameRecord(data, 10000, 30);
        appendFrameRecord(data, 20000, 60);

        QVERIFY(reader.blockProcessing(5020, 1000, data));
        QCOMPARE(reader.getFrameRate(), 20.0);
        QVERIFY(reader.hasFrameMapping());
        QVERIFY(reader.loadHeaders());

        const std::vector<NS_TTSReader::Point> &points = reader.getPoints();
        QCOMPARE(points.size(), size_t(3));
        QCOMPARE(points[0].getTime(), 0.0);
        QCOMPARE(points[1].getTime(), 1500.0);
        QCOMPARE(points[2].getTime(), 3000.0);
        for (const NS_TTSReader::Point &point : points)
            QVERIFY(std::isfinite(point.getTime()));
    }

    void decodesUnalignedProgramRecordAsLittleEndian()
    {
        TTSReader reader;
        ByteArray data = {
            byte(0x06), byte(0xff),             // slope: -250 / 100
            byte(0x10), byte(0x27), byte(0), byte(0) // distance: 10000
        };

        QVERIFY(reinterpret_cast<std::uintptr_t>(data.data() + 2)
                % alignof(std::uint32_t) != 0);
        QVERIFY(reader.blockProcessing(1032, 1000, data));

        QCOMPARE(reader.getMinSlope(), -2.5);
        QCOMPARE(reader.getMaxSlope(), -2.5);
    }

    void decodesUnalignedSegmentRecordAsLittleEndian()
    {
        TTSReader reader;
        ByteArray data = {
            byte(1), byte(0),
            byte(0xa0), byte(0x86), byte(0x01), byte(0),
            byte(0x40), byte(0x0d), byte(0x03), byte(0)
        };

        QVERIFY(reinterpret_cast<std::uintptr_t>(data.data() + 2)
                % alignof(std::uint32_t) != 0);
        QVERIFY(reader.blockProcessing(1041, 1000, data));
        QVERIFY(reader.flushPendingSegment());

        const std::vector<NS_TTSReader::Segment> &segments =
            reader.getSegments();
        QCOMPARE(segments.size(), size_t(1));
        QCOMPARE(segments.front().startKM, 1.0);
        QCOMPARE(segments.front().endKM, 2.0);
    }

    void rejectsZeroSegmentDivisorAndNonFiniteSegments()
    {
        TTSReader reader;
        ByteArray data;
        appendUInt16(data, 0);
        appendUInt32(data, 100000);
        appendUInt32(data, 200000);

        QVERIFY(!reader.blockProcessing(1041, 1000, data));
        QVERIFY(!reader.flushPendingSegment());
        QVERIFY(reader.getSegments().empty());

        NS_TTSReader::Segment segment;
        segment.startKM = std::numeric_limits<double>::infinity();
        segment.endKM = 1.0;
        QVERIFY(!segment.IsValid());
    }

    void decodesNonzeroGpsIeee754Values()
    {
        constexpr float latitude = 60.1699f;
        constexpr float longitude = 24.9384f;
        constexpr float altitude = 42.5f;

        TTSReader reader;
        ByteArray data;
        appendGpsRecord(data, 12345, latitude, longitude, altitude);

        QVERIFY(reader.blockProcessing(5050, 1000, data));
        QVERIFY(reader.loadHeaders());

        const std::vector<NS_TTSReader::Point> &points = reader.getPoints();
        QCOMPARE(points.size(), size_t(1));
        QCOMPARE(points.front().getLatitude(), static_cast<double>(latitude));
        QCOMPARE(points.front().getLongitude(), static_cast<double>(longitude));
        QCOMPARE(points.front().getElevation(), static_cast<double>(altitude));
        QVERIFY(qFuzzyCompare(points.front().getDistanceFromStart(), 123.45));
        QVERIFY(reader.hasGPS());
        QVERIFY(reader.hasElevation());
    }

    void rejectsNonFiniteTelemetryTransactionally()
    {
        QByteArray fixture;
        ByteArray gps;
        appendGpsRecord(gps, 12345,
                        std::numeric_limits<float>::quiet_NaN(),
                        24.9384f, 42.5f);
        appendEncryptedBlock(fixture, 1, 5050, 1000, gps);

        TTSReader reader;
        QVERIFY(!parseFixture(reader, fixture));
        QVERIFY(reader.getPoints().empty());
    }

    void decodesSegmentRangeFieldsWithoutOverlap()
    {
        TTSReader reader;
        ByteArray data;
        appendUInt32(data, 123400);
        appendUInt32(data, 567800);
        appendUInt16(data, 0xbeef);

        QVERIFY(reader.blockProcessing(1050, 1000, data));

        const std::vector<NS_TTSReader::SegmentRange> &ranges =
            reader.getSegmentRanges();
        QCOMPARE(ranges.size(), size_t(1));
        QVERIFY(qFuzzyCompare(ranges.front().startKM, 1.234));
        QVERIFY(qFuzzyCompare(ranges.front().endKM, 5.678));
        QCOMPARE(ranges.front().metadata, std::uint16_t(0xbeef));
    }

    void parsesRealisticEncryptedFixtureEndToEnd()
    {
        TTSReader reader;

        QVERIFY(parseFixture(reader, realisticFixture()));

        const std::vector<NS_TTSReader::Point> &points = reader.getPoints();
        QCOMPARE(points.size(), size_t(3));
        QCOMPARE(reader.getFrameRate(), 20.0);
        QCOMPARE(reader.getTotalDistance(), 200.0);
        QCOMPARE(points.front().getLatitude(), static_cast<double>(60.1699f));
        QCOMPARE(points.back().getLongitude(), static_cast<double>(24.9400f));
        QVERIFY(reader.hasKm());
        QVERIFY(reader.hasGPS());
        QVERIFY(reader.hasElevation());
        QVERIFY(reader.hasGradient());
        QVERIFY(reader.hasFrameMapping());

        const std::vector<NS_TTSReader::SegmentRange> &ranges =
            reader.getSegmentRanges();
        QCOMPARE(ranges.size(), size_t(1));
        QCOMPARE(ranges.front().metadata, std::uint16_t(0x1234));

        for (const NS_TTSReader::Point &point : points) {
            QVERIFY(std::isfinite(point.getLatitude()));
            QVERIFY(std::isfinite(point.getLongitude()));
            QVERIFY(std::isfinite(point.getElevation()));
            QVERIFY(std::isfinite(point.getGradient()));
            QVERIFY(std::isfinite(point.getTime()));
            QVERIFY(std::isfinite(point.getSpeed()));
        }
    }

    void parsesEncryptedPayloadBeginningWithHeaderBytes()
    {
        TTSReader reader;
        const QByteArray fixture = headerLikeCiphertextFixture();

        QCOMPARE(static_cast<UByte>(fixture[14]), UByte(1));
        QCOMPARE(static_cast<UByte>(fixture[15]), UByte(0));
        QVERIFY(parseFixture(reader, fixture));
        QCOMPARE(reader.getPoints().size(), size_t(2));
        QVERIFY(reader.hasGradient());
    }

    void parsesEncryptedPayloadAcrossDataKeyWraps()
    {
        ByteArray program;
        for (int i = 0; i < 44; ++i)
            appendProgramRecord(program, 250, 100);

        QVERIFY(program.size() > 128);
        QVERIFY(program.size() > 2 * ttsDataKey.size());

        QByteArray fixture;
        appendEncryptedBlock(fixture, 1, 1032, 1000, program);

        TTSReader reader;
        QVERIFY(parseFixture(reader, fixture));

        const std::vector<NS_TTSReader::Point> &points = reader.getPoints();
        QCOMPARE(points.size(), size_t(45));
        QCOMPARE(reader.getMinSlope(), 2.5);
        QCOMPARE(reader.getMaxSlope(), 2.5);
        QCOMPARE(reader.getTotalDistance(), 44.0);
        for (size_t i = 1; i < points.size(); ++i) {
            QCOMPARE(points[i].getGradient(), 2.5);
            QCOMPARE(points[i].getDistanceFromStart(), static_cast<double>(i));
        }
    }

    void decodesAndOverwritesUtf16RouteName()
    {
        QByteArray fixture;
        const ByteArray empty;
        appendEncryptedBlock(fixture, 1, 1000, 1000, empty);

        ByteArray firstName;
        firstName.reserve(4096 * 2);
        for (int i = 0; i < 4096; ++i)
            appendUInt16(firstName, 'A');
        appendEncryptedBlock(fixture, 1, 110, 1000, firstName);

        ByteArray replacementName;
        replacementName.reserve(8192 * 2);
        for (int i = 0; i < 8192; ++i)
            appendUInt16(replacementName, 'B');
        appendEncryptedBlock(fixture, 1, 110, 1000, replacementName);

        ByteArray program;
        appendProgramRecord(program, 100, 0);
        appendProgramRecord(program, 100, 100);
        appendEncryptedBlock(fixture, 2, 1032, 1000, program);

        TTSReader reader;
        QVERIFY(parseFixture(reader, fixture));
        QVERIFY(reader.getRouteName() == std::wstring(8192, L'B'));
        QCOMPARE(reader.getPoints().size(), size_t(2));
    }

    void rejectsWrappedAndOversizedHeaderDimensions_data()
    {
        QTest::addColumn<std::uint32_t>("elementSize");
        QTest::addColumn<std::uint32_t>("elementCount");
        QTest::addColumn<int>("wrappedPayloadSize");

        QTest::newRow("32-bit-product-wraps-to-four")
            << std::uint32_t(0x40000001U) << std::uint32_t(4) << 4;
        QTest::newRow("single-block-over-practical-limit")
            << std::uint32_t(256U * 1024U * 1024U + 1U)
            << std::uint32_t(1) << 0;
    }

    void rejectsWrappedAndOversizedHeaderDimensions()
    {
        QFETCH(std::uint32_t, elementSize);
        QFETCH(std::uint32_t, elementCount);
        QFETCH(int, wrappedPayloadSize);

        QByteArray fixture;
        const ByteArray payload(static_cast<size_t>(wrappedPayloadSize), byte(0));
        appendEncryptedBlockWithDimensions(fixture, 1, 1031, 1000,
                                           elementSize, elementCount, payload);

        TTSReader reader;
        QVERIFY(!parseFixture(reader, fixture));
        QVERIFY(reader.getPoints().empty());
    }

    void rejectsDecodedMemoryAmplificationBeforePayloadRead()
    {
        constexpr std::uint32_t frameRecords = 16U * 1024U * 1024U;
        const ByteArray header = makeHeader(1, 5020, 1000, 8, frameRecords);
        PrefixOnlySequentialDevice device(
            QByteArray(header.data(), static_cast<int>(header.size())));
        QVERIFY(device.open(QIODevice::ReadOnly
                            | QIODevice::Unbuffered));

        QDataStream stream(&device);
        stream.setByteOrder(QDataStream::LittleEndian);

        TTSReader reader;
        QVERIFY(!reader.parseFile(stream));
        QCOMPARE(device.postPrefixReadRequests(), 0);
        QVERIFY(reader.getPoints().empty());
    }

    void rejectsCumulativeDecodedWorkingSetBeforePayloadRead()
    {
        constexpr std::uint32_t frameRecords = 2'000'000;
        constexpr std::uint32_t gpsRecords = 1'000'000;

        const ByteArray frameHeader =
            makeHeader(1, 5020, 1000, 8, frameRecords);
        QByteArray prefix(frameHeader.data(),
                          static_cast<int>(frameHeader.size()));
        prefix.append(QByteArray(static_cast<int>(frameRecords * 8U), '\0'));

        const ByteArray gpsHeader =
            makeHeader(2, 5050, 1000, 16, gpsRecords);
        prefix.append(gpsHeader.data(), static_cast<int>(gpsHeader.size()));

        PrefixOnlySequentialDevice device(std::move(prefix));
        QVERIFY(device.open(QIODevice::ReadOnly
                            | QIODevice::Unbuffered));

        QDataStream stream(&device);
        stream.setByteOrder(QDataStream::LittleEndian);

        TTSReader reader;
        QVERIFY(!reader.parseFile(stream));
        QCOMPARE(device.postPrefixReadRequests(), 0);
        QVERIFY(reader.getPoints().empty());
    }

    void rejectsOversizedUtf16BeforePayloadRead()
    {
        constexpr std::uint32_t oversizedUtf16Bytes = 1024U * 1024U + 2U;
        const ByteArray header =
            makeHeader(1, 110, 1000, oversizedUtf16Bytes, 1);
        PrefixOnlySequentialDevice device(
            QByteArray(header.data(), static_cast<int>(header.size())));
        QVERIFY(device.open(QIODevice::ReadOnly
                            | QIODevice::Unbuffered));

        QDataStream stream(&device);
        stream.setByteOrder(QDataStream::LittleEndian);

        TTSReader reader;
        QVERIFY(!reader.parseFile(stream));
        QCOMPARE(device.postPrefixReadRequests(), 0);
        QVERIFY(reader.getPoints().empty());
    }

    void propagatesBlockFailureThroughLoadHeadersAndParse()
    {
        TTSReader reader;

        QVERIFY(!parseFixture(reader, malformedFixture()));
        QVERIFY(reader.getPoints().empty());
    }

    void parseFailureIsTransactionalAndReaderIsReusable()
    {
        TTSReader reader;
        QVERIFY(parseFixture(reader, realisticFixture()));

        const size_t originalPointCount = reader.getPoints().size();
        const size_t originalRangeCount = reader.getSegmentRanges().size();
        const double originalDistance = reader.getTotalDistance();
        const double originalFrameRate = reader.getFrameRate();

        QVERIFY(!parseFixture(reader, lateMalformedFixture()));
        QCOMPARE(reader.getPoints().size(), originalPointCount);
        QCOMPARE(reader.getSegmentRanges().size(), originalRangeCount);
        QCOMPARE(reader.getTotalDistance(), originalDistance);
        QCOMPARE(reader.getFrameRate(), originalFrameRate);

        QVERIFY(parseFixture(reader, realisticFixture()));
        QCOMPARE(reader.getPoints().size(), originalPointCount);
        QCOMPARE(reader.getSegmentRanges().size(), originalRangeCount);
    }

    void rejectsOneByteTrailingHeaderPrefix()
    {
        QByteArray fixture = realisticFixture();
        fixture.append(char(1));

        TTSReader reader;
        QVERIFY(!parseFixture(reader, fixture));
        QVERIFY(reader.getPoints().empty());
    }

    void rejectsEmptyFileWithoutIndexingEmptyPoints()
    {
        TTSReader reader;

        QVERIFY(!parseFixture(reader, QByteArray()));
        QVERIFY(reader.getPoints().empty());
        QCOMPARE(reader.getFrameRate(), 0.0);
    }
};

QTEST_GUILESS_MAIN(TestTTSReaderBounds)
#include "testTTSReaderBounds.moc"
