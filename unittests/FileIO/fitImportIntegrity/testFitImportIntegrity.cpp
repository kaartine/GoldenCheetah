/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "FileIO/FitFileIntegrity.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTest>

#include <limits>

namespace {

quint16 referenceCrc(const QByteArray &bytes)
{
    static const quint16 table[16] = {
        0x0000, 0xCC01, 0xD801, 0x1400,
        0xF001, 0x3C00, 0x2800, 0xE401,
        0xA001, 0x6C00, 0x7800, 0xB401,
        0x5000, 0x9C01, 0x8801, 0x4400
    };

    quint16 crc = 0;
    for (const char value : bytes) {
        const quint8 byte = static_cast<quint8>(value);
        quint16 temporary = table[crc & 0x0f];
        crc = static_cast<quint16>((crc >> 4) & 0x0fff);
        crc = static_cast<quint16>(
            crc ^ temporary ^ table[byte & 0x0f]);
        temporary = table[crc & 0x0f];
        crc = static_cast<quint16>((crc >> 4) & 0x0fff);
        crc = static_cast<quint16>(
            crc ^ temporary ^ table[(byte >> 4) & 0x0f]);
    }
    return crc;
}

void appendLittleEndian16(QByteArray &bytes, quint16 value)
{
    bytes.append(static_cast<char>(value & 0xff));
    bytes.append(static_cast<char>((value >> 8) & 0xff));
}

void writeLittleEndian32(QByteArray &bytes, int offset, quint32 value)
{
    for (int index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<char>(
            (value >> (index * 8)) & 0xff);
    }
}

void rewriteFileCrc(QByteArray &bytes)
{
    bytes.chop(2);
    appendLittleEndian16(bytes, referenceCrc(bytes));
}

QByteArray fitSegment(int headerSize,
                      const QByteArray &data = {},
                      bool zeroHeaderCrc = false)
{
    QByteArray header(headerSize, '\0');
    header[0] = static_cast<char>(headerSize);
    header[1] = static_cast<char>(0x20);
    writeLittleEndian32(
        header, 4, static_cast<quint32>(data.size()));
    header.replace(8, 4, QByteArrayLiteral(".FIT"));
    if (headerSize == 14 && !zeroHeaderCrc) {
        const quint16 crc = referenceCrc(header.left(12));
        header[12] = static_cast<char>(crc & 0xff);
        header[13] = static_cast<char>((crc >> 8) & 0xff);
    }

    QByteArray result = header + data;
    appendLittleEndian16(result, referenceCrc(result));
    return result;
}

FitFileIntegrity::ValidationResult validate(
    const QByteArray &contents,
    qint64 maximumFileSize =
        FitFileIntegrity::MaximumFileSize)
{
    QByteArray copy = contents;
    QBuffer buffer(&copy);
    if (!buffer.open(QIODevice::ReadOnly)) {
        qFatal("Could not open FIT integrity test buffer");
    }
    return FitFileIntegrity::validate(
        buffer, maximumFileSize);
}

} // namespace

class TestFitImportIntegrity : public QObject
{
    Q_OBJECT

private slots:
    void acceptsValidFiles_data();
    void acceptsValidFiles();
    void acceptsRepositoryFixtures_data();
    void acceptsRepositoryFixtures();
    void acceptsAllRepositoryFixtures_data();
    void acceptsAllRepositoryFixtures();
    void rejectsHeaderErrors_data();
    void rejectsHeaderErrors();
    void rejectsDataAndFileCrcErrors_data();
    void rejectsDataAndFileCrcErrors();
    void rejectsEveryTruncation_data();
    void rejectsEveryTruncation();
    void enforcesPhysicalSizeLimit();
    void restoresDevicePosition();
    void rejectsClosedDevice();
    void accountsForRecordBytes_data();
    void accountsForRecordBytes();
};

void TestFitImportIntegrity::acceptsValidFiles_data()
{
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<int>("segments");

    QTest::newRow("legacy-header")
        << fitSegment(12) << 1;
    QTest::newRow("current-header")
        << fitSegment(14) << 1;
    QTest::newRow("optional-zero-header-crc")
        << fitSegment(14, {}, true) << 1;
    QTest::newRow("opaque-data")
        << fitSegment(14, QByteArray::fromHex("01020300ff"))
        << 1;
    QTest::newRow("chained-files")
        << fitSegment(12, QByteArray::fromHex("0102"))
               + fitSegment(14, QByteArray::fromHex("030405"))
        << 2;
}

void TestFitImportIntegrity::acceptsValidFiles()
{
    QFETCH(QByteArray, contents);
    QFETCH(int, segments);

    const auto result = validate(contents);

    QVERIFY2(result.valid, qPrintable(result.error));
    QCOMPARE(result.segmentCount, segments);
}

void TestFitImportIntegrity::acceptsRepositoryFixtures_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<int>("segments");

    const QDir repository(QStringLiteral(GC_TEST_SOURCE_ROOT));
    const QString cycling = repository.filePath(
        QStringLiteral("test/rides/20130717_143733.fit"));
    const QString swimming = repository.filePath(
        QStringLiteral("test/swims/GarminHRMswim.FIT"));
    const QString recentGarmin = repository.filePath(
        QStringLiteral("test/roundtrip/Fr955v19.28andStryd.fit"));

    QVERIFY(QFile::exists(cycling));
    QVERIFY(QFile::exists(swimming));
    QVERIFY(QFile::exists(recentGarmin));
    QTest::newRow("cycling") << cycling << 1;
    QTest::newRow("swimming-chained") << swimming << 3;
    QTest::newRow("recent-garmin") << recentGarmin << 1;
}

void TestFitImportIntegrity::acceptsRepositoryFixtures()
{
    QFETCH(QString, path);
    QFETCH(int, segments);
    QFile file(path);
    QVERIFY2(file.open(QIODevice::ReadOnly),
             qPrintable(file.errorString()));

    const auto result = FitFileIntegrity::validate(file);

    QVERIFY2(result.valid, qPrintable(result.error));
    QCOMPARE(result.segmentCount, segments);
}

void TestFitImportIntegrity::acceptsAllRepositoryFixtures_data()
{
    QTest::addColumn<QString>("path");

    const QString root = QDir(QStringLiteral(GC_TEST_SOURCE_ROOT)).filePath(
        QStringLiteral("test"));
    QVERIFY(QDir(root).exists());
    QDirIterator files(
        root,
        {QStringLiteral("*.fit"), QStringLiteral("*.FIT")},
        QDir::Files,
        QDirIterator::Subdirectories);

    int fixtureCount = 0;
    while (files.hasNext()) {
        const QString path = files.next();
        const QByteArray row = QDir(root).relativeFilePath(path).toUtf8();
        QTest::newRow(row.constData()) << path;
        ++fixtureCount;
    }
    QVERIFY(fixtureCount > 0);
}

void TestFitImportIntegrity::acceptsAllRepositoryFixtures()
{
    QFETCH(QString, path);
    QFile file(path);
    QVERIFY2(file.open(QIODevice::ReadOnly),
             qPrintable(file.errorString()));

    const auto result = FitFileIntegrity::validate(file);

    QVERIFY2(result.valid,
             qPrintable(QStringLiteral("%1: %2")
                            .arg(path, result.error)));
}

void TestFitImportIntegrity::rejectsHeaderErrors_data()
{
    QTest::addColumn<QByteArray>("contents");

    QTest::newRow("empty") << QByteArray();

    QByteArray tooShort = fitSegment(12);
    tooShort[0] = static_cast<char>(11);
    QTest::newRow("header-size-below-minimum") << tooShort;

    QByteArray unsupported = fitSegment(12);
    unsupported[0] = static_cast<char>(13);
    QTest::newRow("unsupported-header-size") << unsupported;

    QByteArray badSignature = fitSegment(12);
    badSignature[8] = 'X';
    rewriteFileCrc(badSignature);
    QTest::newRow("bad-signature") << badSignature;

    QByteArray badHeaderCrc = fitSegment(14);
    badHeaderCrc[12] ^= 0x01;
    rewriteFileCrc(badHeaderCrc);
    QTest::newRow("bad-header-crc") << badHeaderCrc;
}

void TestFitImportIntegrity::rejectsHeaderErrors()
{
    QFETCH(QByteArray, contents);

    const auto result = validate(contents);

    QVERIFY(result.valid == false);
    QVERIFY(!result.error.isEmpty());
}

void TestFitImportIntegrity::rejectsDataAndFileCrcErrors_data()
{
    QTest::addColumn<QByteArray>("contents");

    QByteArray declaredTooLarge =
        fitSegment(12, QByteArray::fromHex("010203"));
    writeLittleEndian32(declaredTooLarge, 4, 4);
    QTest::newRow("declared-data-too-large")
        << declaredTooLarge;

    QByteArray declaredTooSmall =
        fitSegment(12, QByteArray::fromHex("010203"));
    writeLittleEndian32(declaredTooSmall, 4, 2);
    QTest::newRow("declared-data-too-small")
        << declaredTooSmall;

    QByteArray corruptData =
        fitSegment(14, QByteArray::fromHex("010203"));
    corruptData[14] ^= 0x01;
    QTest::newRow("bad-file-crc") << corruptData;

    QByteArray corruptStoredCrc = fitSegment(14);
    corruptStoredCrc[corruptStoredCrc.size() - 1] ^= 0x01;
    QTest::newRow("corrupt-stored-crc")
        << corruptStoredCrc;

    QTest::newRow("trailing-byte")
        << fitSegment(14) + QByteArray(1, '\0');

    QByteArray badSecond = fitSegment(12) + fitSegment(14);
    badSecond[badSecond.size() - 1] ^= 0x01;
    QTest::newRow("bad-second-segment") << badSecond;
}

void TestFitImportIntegrity::rejectsDataAndFileCrcErrors()
{
    QFETCH(QByteArray, contents);

    const auto result = validate(contents);

    QVERIFY(result.valid == false);
    QVERIFY(!result.error.isEmpty());
}

void TestFitImportIntegrity::rejectsEveryTruncation_data()
{
    QTest::addColumn<QByteArray>("contents");
    const QByteArray complete = fitSegment(
        14, QByteArray::fromHex("010203040506"));
    for (int size = 0; size < complete.size(); ++size) {
        const QByteArray name = QByteArray("size-")
            + QByteArray::number(size);
        QTest::newRow(name.constData()) << complete.left(size);
    }
}

void TestFitImportIntegrity::rejectsEveryTruncation()
{
    QFETCH(QByteArray, contents);

    const auto result = validate(contents);

    QVERIFY(result.valid == false);
    QVERIFY(!result.error.isEmpty());
}

void TestFitImportIntegrity::enforcesPhysicalSizeLimit()
{
    const QByteArray contents = fitSegment(14);

    QVERIFY(validate(contents, contents.size()).valid);
    const auto result = validate(
        contents, contents.size() - 1);
    QVERIFY(result.valid == false);
    QVERIFY(!result.error.isEmpty());
}

void TestFitImportIntegrity::restoresDevicePosition()
{
    QByteArray contents = fitSegment(14);
    QBuffer buffer(&contents);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    QVERIFY(buffer.seek(5));

    const auto result = FitFileIntegrity::validate(buffer);

    QVERIFY2(result.valid, qPrintable(result.error));
    QCOMPARE(buffer.pos(), qint64(5));
}

void TestFitImportIntegrity::rejectsClosedDevice()
{
    QByteArray contents = fitSegment(14);
    QBuffer buffer(&contents);

    const auto result = FitFileIntegrity::validate(buffer);

    QVERIFY(result.valid == false);
    QVERIFY(!result.error.isEmpty());
}

void TestFitImportIntegrity::accountsForRecordBytes_data()
{
    QTest::addColumn<qint64>("remaining");
    QTest::addColumn<qint64>("consumed");
    QTest::addColumn<bool>("accepted");
    QTest::addColumn<qint64>("resulting");

    QTest::newRow("partial")
        << qint64(5) << qint64(1) << true << qint64(4);
    QTest::newRow("exact")
        << qint64(5) << qint64(5) << true << qint64(0);
    QTest::newRow("overshoot")
        << qint64(5) << qint64(6) << false << qint64(5);
    QTest::newRow("zero")
        << qint64(5) << qint64(0) << false << qint64(5);
    QTest::newRow("negative")
        << qint64(5) << qint64(-1) << false << qint64(5);
    QTest::newRow("already-complete")
        << qint64(0) << qint64(1) << false << qint64(0);
    QTest::newRow("maximum")
        << std::numeric_limits<qint64>::max()
        << std::numeric_limits<qint64>::max()
        << true << qint64(0);
}

void TestFitImportIntegrity::accountsForRecordBytes()
{
    QFETCH(qint64, remaining);
    QFETCH(qint64, consumed);
    QFETCH(bool, accepted);
    QFETCH(qint64, resulting);

    QCOMPARE(FitFileIntegrity::consumeRecordBytes(
                 remaining, consumed),
             accepted);
    QCOMPARE(remaining, resulting);
}

QTEST_GUILESS_MAIN(TestFitImportIntegrity)
#include "testFitImportIntegrity.moc"
