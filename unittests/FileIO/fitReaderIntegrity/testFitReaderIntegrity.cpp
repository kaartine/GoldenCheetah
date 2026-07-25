/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "FitFileIntegrity.h"
#include "FitRideFile.h"

#include <QBuffer>
#include <QFile>
#include <QTemporaryFile>
#include <QTest>

#include <memory>

extern bool loaded;
extern QStringList FITbasetypes;
extern QList<FITproduct> FITproducts;
extern QList<FITmanufacturer> FITmanufacturers;
extern QList<FITmessage> FITmessages;
extern QList<FitFieldDefinition> FITstandardfields;
extern QStringList GenericDecodeList;

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

QByteArray fitSegment(const QByteArray &records)
{
    QByteArray header(14, '\0');
    header[0] = 14;
    header[1] = 0x20;
    writeLittleEndian32(
        header, 4, static_cast<quint32>(records.size()));
    header.replace(8, 4, QByteArrayLiteral(".FIT"));
    const quint16 headerCrc = referenceCrc(header.left(12));
    header[12] = static_cast<char>(headerCrc & 0xff);
    header[13] = static_cast<char>((headerCrc >> 8) & 0xff);

    QByteArray result = header + records;
    appendLittleEndian16(result, referenceCrc(result));
    return result;
}

RideFile *importBytes(const QByteArray &contents,
                      QStringList &errors,
                      bool *inputClosed = nullptr)
{
    QTemporaryFile temporary;
    if (!temporary.open())
        qFatal("Could not create a temporary FIT file");
    if (temporary.write(contents) != contents.size())
        qFatal("Could not write a temporary FIT file");
    temporary.close();

    QFile input(temporary.fileName());
    FitFileReader reader;
    RideFile *result = reader.openRideFile(input, errors);
    if (inputClosed)
        *inputClosed = !input.isOpen();
    return result;
}

} // namespace

class TestFitReaderIntegrity : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void importsValidRecordsAndChainedSegments();
    void importsRepositoryFixtures_data();
    void importsRepositoryFixtures();
    void rejectsSemanticRecordOverrun();
    void rejectsCorruptFileCrc();
    void rejectsEveryTruncation();
};

void TestFitReaderIntegrity::initTestCase()
{
    loaded = true;
    FITbasetypes = {
        QStringLiteral("enum"), QStringLiteral("sint8"),
        QStringLiteral("uint8"), QStringLiteral("sint16"),
        QStringLiteral("uint16"), QStringLiteral("sint32"),
        QStringLiteral("uint32"), QStringLiteral("string"),
        QStringLiteral("float32"), QStringLiteral("float64"),
        QStringLiteral("uint8z"), QStringLiteral("uint16z"),
        QStringLiteral("uint32z"), QStringLiteral("byte"),
        QStringLiteral("sint64"), QStringLiteral("uint64"),
        QStringLiteral("uint64z")
    };
    FITproducts.clear();
    FITmanufacturers.clear();
    FITmessages.clear();
    FITstandardfields.clear();
    GenericDecodeList.clear();
}

void TestFitReaderIntegrity::importsValidRecordsAndChainedSegments()
{
    const QByteArray records = QByteArray::fromHex("40000071000000");
    const QByteArray segment = fitSegment(records);
    QStringList errors;
    bool inputClosed = false;

    std::unique_ptr<RideFile> ride(
        importBytes(segment + segment, errors, &inputClosed));

    QVERIFY2(ride != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
    QVERIFY(errors.isEmpty());
    QVERIFY(inputClosed);
    QCOMPARE(ride->deviceType(), QStringLiteral("Garmin FIT"));
}

void TestFitReaderIntegrity::importsRepositoryFixtures_data()
{
    QTest::addColumn<QString>("path");

    const QString cycling = QFINDTESTDATA(
        "../../../test/rides/20130717_143733.fit");
    const QString swimming = QFINDTESTDATA(
        "../../../test/swims/GarminHRMswim.FIT");
    QVERIFY(!cycling.isEmpty());
    QVERIFY(!swimming.isEmpty());
    QTest::newRow("cycling") << cycling;
    QTest::newRow("swimming-chained") << swimming;
}

void TestFitReaderIntegrity::importsRepositoryFixtures()
{
    QFETCH(QString, path);
    QFile input(path);
    QStringList errors;
    FitFileReader reader;

    std::unique_ptr<RideFile> ride(reader.openRideFile(input, errors));

    QVERIFY2(ride != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
    QVERIFY(!input.isOpen());
    QVERIFY(!ride->dataPoints().isEmpty());
}

void TestFitReaderIntegrity::rejectsSemanticRecordOverrun()
{
    const QByteArray records = QByteArray::fromHex(
        "40000071000100048600");
    const QByteArray contents = fitSegment(records);
    QByteArray validationCopy = contents;
    QBuffer buffer(&validationCopy);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    QVERIFY2(FitFileIntegrity::validate(buffer).valid,
             "The parser regression input must be CRC-valid");
    QStringList errors;
    bool inputClosed = false;

    std::unique_ptr<RideFile> ride(
        importBytes(contents, errors, &inputClosed));

    QVERIFY(ride == nullptr);
    QVERIFY(!errors.isEmpty());
    QVERIFY(inputClosed);
}

void TestFitReaderIntegrity::rejectsCorruptFileCrc()
{
    QByteArray contents = fitSegment(
        QByteArray::fromHex("40000071000000"));
    contents[contents.size() - 1] = static_cast<char>(
        contents.at(contents.size() - 1) ^ 0x01);
    QStringList errors;

    std::unique_ptr<RideFile> ride(importBytes(contents, errors));

    QVERIFY(ride == nullptr);
    QVERIFY(errors.join(QLatin1Char('\n')).contains(
        QStringLiteral("CRC")));
}

void TestFitReaderIntegrity::rejectsEveryTruncation()
{
    const QByteArray complete = fitSegment(
        QByteArray::fromHex("40000071000000"));
    for (int size = 0; size < complete.size(); ++size) {
        QStringList errors;
        std::unique_ptr<RideFile> ride(
            importBytes(complete.left(size), errors));
        QVERIFY2(ride == nullptr,
                 qPrintable(QStringLiteral("accepted %1 bytes").arg(size)));
        QVERIFY2(!errors.isEmpty(),
                 qPrintable(QStringLiteral("no error at %1 bytes").arg(size)));
    }
}

QTEST_GUILESS_MAIN(TestFitReaderIntegrity)
#include "testFitReaderIntegrity.moc"
