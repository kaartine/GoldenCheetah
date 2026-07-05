/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "CafTestStubs.h"

#include <QtEndian>
#include <QScopedPointer>
#include <QTemporaryFile>
#include <QTest>

#include <cstring>
#include <limits>

#ifndef QT_NO_DEBUG
#error "tacxCafBounds must exercise the importer with release assertions disabled"
#endif

class TestTacxCafBounds : public QObject
{
    Q_OBJECT

    struct ParseResult {
        bool imported = false;
        QStringList errors;
        QList<RideFile::AppendedPoint> points;
        QDateTime startTime;
        double recordingInterval = 0.0;
    };

    template<typename T>
    static void appendValue(QByteArray &bytes, T value)
    {
        const T littleEndianValue = qToLittleEndian(value);
        bytes.append(reinterpret_cast<const char *>(&littleEndianValue),
                     sizeof(littleEndianValue));
    }

    template<typename T>
    static void writeValue(QByteArray &bytes, qsizetype offset, T value)
    {
        Q_ASSERT(offset >= 0
                 && offset + qsizetype(sizeof(value)) <= bytes.size());
        const T littleEndianValue = qToLittleEndian(value);
        std::memcpy(bytes.data() + offset, &littleEndianValue, sizeof(value));
    }

    static void writeFloat(QByteArray &bytes, qsizetype offset, float value)
    {
        quint32 bits;
        static_assert(sizeof(bits) == sizeof(value), "Unexpected float size");
        std::memcpy(&bits, &value, sizeof(bits));
        writeValue<quint32>(bytes, offset, bits);
    }

    static int rideInformationRecordSize(qint16 version)
    {
        return version == 100 ? 202 : 236;
    }

    static int rideDataRecordSize(qint16 version)
    {
        return version == 100 ? 10 : 18;
    }

    static QByteArray fileHeader(qint16 version, qint32 blockCount)
    {
        QByteArray bytes;
        appendValue<qint16>(bytes, 3000);
        appendValue<qint16>(bytes, version);
        appendValue<qint32>(bytes, blockCount);
        return bytes;
    }

    static void appendBlockHeader(QByteArray &bytes, qint16 fingerprint,
                                  qint16 blockVersion, qint32 recordCount,
                                  qint32 recordSize)
    {
        appendValue<qint16>(bytes, fingerprint);
        appendValue<qint16>(bytes, blockVersion);
        appendValue<qint32>(bytes, recordCount);
        appendValue<qint32>(bytes, recordSize);
    }

    static QByteArray rideInformationRecord(qint16 version)
    {
        QByteArray record(rideInformationRecordSize(version), '\0');
        writeValue<qint16>(record, 0, 2026);
        writeValue<qint16>(record, 2, 7);
        writeValue<qint16>(record, 4, 1);
        writeValue<qint16>(record, 6, 5);
        writeValue<qint16>(record, 8, 10);
        writeValue<qint16>(record, 10, 30);
        writeValue<qint16>(record, 12, 15);
        writeValue<qint16>(record, 14, 500);
        writeFloat(record, 16, 2.0f);
        record[record.size() - 1] = char(0x5a);
        return record;
    }

    static QByteArray rideDataRecord(qint16 version, float distance,
                                     quint8 heartRate, quint8 cadence,
                                     quint16 powerX10, quint16 speedX10)
    {
        QByteArray record(rideDataRecordSize(version), '\0');
        writeFloat(record, 0, distance);
        writeValue<quint8>(record, 4, heartRate);
        writeValue<quint8>(record, 5, cadence);
        writeValue<quint16>(record, 6, powerX10);
        writeValue<quint16>(record, 8, speedX10);
        for (qsizetype i = 10; i < record.size(); ++i)
            record[i] = char(0x40 + i);
        return record;
    }

    static void appendRideInformationBlock(QByteArray &bytes, qint16 version)
    {
        const QByteArray record = rideInformationRecord(version);
        appendBlockHeader(bytes, 3010, version, 1, record.size());
        bytes.append(record);
    }

    static void appendRideDataBlock(QByteArray &bytes, qint16 version)
    {
        const int recordSize = rideDataRecordSize(version);
        appendBlockHeader(bytes, 3020, version, 3, recordSize);
        bytes.append(rideDataRecord(version, 100.0f, 120, 80, 2000, 123));
        bytes.append(rideDataRecord(version, 110.0f, 121, 81, 2100, 345));
        bytes.append(rideDataRecord(version, 125.0f, 122, 82, 2250, 567));
    }

    static QByteArray representativeFile(qint16 fileVersion = 100,
                                         qint16 informationVersion = 0,
                                         qint16 dataVersion = 0,
                                         bool includeEmptyCourseBlock = false)
    {
        if (informationVersion == 0)
            informationVersion = fileVersion;
        if (dataVersion == 0)
            dataVersion = fileVersion;

        QByteArray bytes = fileHeader(fileVersion,
                                      includeEmptyCourseBlock ? 3 : 2);
        appendRideInformationBlock(bytes, informationVersion);
        if (includeEmptyCourseBlock)
            appendBlockHeader(bytes, 2040, 100, 0, 0);
        appendRideDataBlock(bytes, dataVersion);
        return bytes;
    }

    static QByteArray fileWithBlock(qint16 fingerprint, qint32 recordCount,
                                    qint32 recordSize,
                                    const QByteArray &payload = QByteArray(),
                                    qint16 fileVersion = 100,
                                    qint16 blockVersion = 0)
    {
        if (blockVersion == 0)
            blockVersion = fileVersion;
        QByteArray bytes = fileHeader(fileVersion, 1);
        appendBlockHeader(bytes, fingerprint, blockVersion,
                          recordCount, recordSize);
        bytes.append(payload);
        return bytes;
    }

    static QByteArray fileWithRideDataBlock(qint32 recordCount,
                                            qint32 recordSize,
                                            const QByteArray &payload,
                                            qint16 fileVersion = 100,
                                            qint16 blockVersion = 0)
    {
        if (blockVersion == 0)
            blockVersion = fileVersion;
        QByteArray bytes = fileHeader(fileVersion, 2);
        appendRideInformationBlock(bytes, fileVersion);
        appendBlockHeader(bytes, 3020, blockVersion, recordCount, recordSize);
        bytes.append(payload);
        return bytes;
    }

    static ParseResult parse(const QByteArray &contents)
    {
        QTemporaryFile temporary;
        if (!temporary.open()
            || temporary.write(contents) != contents.size()
            || !temporary.flush()) {
            ParseResult result;
            result.errors << "Could not create test file";
            return result;
        }

        const QString fileName = temporary.fileName();
        temporary.close();

        RideFileReader *reader = RideFileFactory::instance().readerForSuffix("caf");
        if (!reader) {
            ParseResult result;
            result.errors << "CAF reader was not registered";
            return result;
        }

        QFile input(fileName);
        QStringList errors;
        QScopedPointer<RideFile> ride(reader->openRideFile(input, errors));

        ParseResult result;
        result.imported = !ride.isNull();
        result.errors = errors;
        if (ride) {
            result.points = ride->points();
            result.startTime = ride->startTime();
            result.recordingInterval = ride->recIntSecs();
        }
        return result;
    }

    static void verifyRejected(const ParseResult &result)
    {
        QVERIFY(!result.imported);
        QVERIFY2(!result.errors.isEmpty(), "Malformed CAF input had no error");
    }

    static bool nearlyEqual(double actual, double expected)
    {
        return qAbs(actual - expected) < 0.000001;
    }

private slots:
    void importsRepresentativeFile_data()
    {
        QTest::addColumn<qint16>("version");
        QTest::newRow("version-100") << qint16(100);
        QTest::newRow("version-110") << qint16(110);
    }

    void importsRepresentativeFile()
    {
        QFETCH(qint16, version);
        const ParseResult result = parse(representativeFile(version));

        QVERIFY2(result.imported, qPrintable(result.errors.join('\n')));
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.startTime,
                 QDateTime(QDate(2026, 7, 5), QTime(10, 30, 15)));
        QVERIFY(nearlyEqual(result.recordingInterval, 2.0));
        QCOMPARE(result.points.size(), 3);

        const RideFile::AppendedPoint first = result.points.at(0);
        QVERIFY(nearlyEqual(first.secs, 2.0));
        QVERIFY(nearlyEqual(first.cadence, 80.0));
        QVERIFY(nearlyEqual(first.heartRate, 120.0));
        QVERIFY(nearlyEqual(first.distance, 0.0));
        QVERIFY(nearlyEqual(first.speed, 12.3));
        QVERIFY(nearlyEqual(first.power, 200.0));

        const RideFile::AppendedPoint second = result.points.at(1);
        QVERIFY(nearlyEqual(second.secs, 4.0));
        QVERIFY(nearlyEqual(second.cadence, 81.0));
        QVERIFY(nearlyEqual(second.heartRate, 121.0));
        QVERIFY(nearlyEqual(second.distance, 0.010));
        QVERIFY(nearlyEqual(second.speed, 34.5));
        QVERIFY(nearlyEqual(second.power, 210.0));

        const RideFile::AppendedPoint third = result.points.at(2);
        QVERIFY(nearlyEqual(third.secs, 6.0));
        QVERIFY(nearlyEqual(third.cadence, 82.0));
        QVERIFY(nearlyEqual(third.heartRate, 122.0));
        QVERIFY(nearlyEqual(third.distance, 0.025));
        QVERIFY(nearlyEqual(third.speed, 56.7));
        QVERIFY(nearlyEqual(third.power, 225.0));
    }

    void importsOptionalEmptyBlock_data()
    {
        QTest::addColumn<qint16>("fileVersion");
        QTest::newRow("file-v100-course-v100") << qint16(100);
        QTest::newRow("file-v110-course-v100") << qint16(110);
    }

    void importsOptionalEmptyBlock()
    {
        QFETCH(qint16, fileVersion);
        const ParseResult result = parse(
            representativeFile(fileVersion, 0, 0, true));
        QVERIFY2(result.imported, qPrintable(result.errors.join('\n')));
        QCOMPARE(result.points.size(), 3);
    }

    void importsMixedBlockVersions_data()
    {
        QTest::addColumn<qint16>("informationVersion");
        QTest::addColumn<qint16>("dataVersion");
        QTest::addColumn<bool>("includeEmptyCourseBlock");

        QTest::newRow("file-v110-info-v100-data-v110")
            << qint16(100) << qint16(110) << false;
        QTest::newRow("file-v110-info-v100-course-v100-data-v110")
            << qint16(100) << qint16(110) << true;
    }

    void importsMixedBlockVersions()
    {
        QFETCH(qint16, informationVersion);
        QFETCH(qint16, dataVersion);
        QFETCH(bool, includeEmptyCourseBlock);

        const ParseResult result = parse(representativeFile(
            110, informationVersion, dataVersion, includeEmptyCourseBlock));
        QVERIFY2(result.imported, qPrintable(result.errors.join('\n')));
        QCOMPARE(result.points.size(), 3);
        QVERIFY(nearlyEqual(result.points.at(1).speed, 34.5));
    }

    void truncatesAtEveryByteBoundary_data()
    {
        QTest::addColumn<qint16>("version");
        QTest::addColumn<int>("cut");

        const qint16 versions[] = { 100, 110 };
        for (qint16 version : versions) {
            const int size = representativeFile(version).size();
            for (int cut = 0; cut < size; ++cut) {
                QTest::newRow(qPrintable(QString("v%1-cut-%2")
                    .arg(version)
                    .arg(cut, 3, 10, QLatin1Char('0'))))
                    << version << cut;
            }
        }
    }

    void truncatesAtEveryByteBoundary()
    {
        QFETCH(qint16, version);
        QFETCH(int, cut);
        verifyRejected(parse(representativeFile(version).left(cut)));
    }

    void rejectsDeclaredBlockBoundaryTruncation_data()
    {
        QTest::addColumn<qint16>("version");
        QTest::addColumn<int>("cut");

        const qint16 versions[] = { 100, 110 };
        for (qint16 version : versions) {
            const int ridePayloadEnd =
                8 + 12 + rideInformationRecordSize(version);
            const int fullSize = representativeFile(version).size();
            QTest::newRow(qPrintable(QString("v%1-ride-header").arg(version)))
                << version << 8 + 11;
            QTest::newRow(qPrintable(QString("v%1-ride-payload").arg(version)))
                << version << ridePayloadEnd - 1;
            QTest::newRow(qPrintable(QString("v%1-data-header").arg(version)))
                << version << ridePayloadEnd + 11;
            QTest::newRow(qPrintable(QString("v%1-data-payload").arg(version)))
                << version << fullSize - 1;
        }
    }

    void rejectsDeclaredBlockBoundaryTruncation()
    {
        QFETCH(qint16, version);
        QFETCH(int, cut);
        verifyRejected(parse(representativeFile(version).left(cut)));
    }

    void rejectsDeclaredBlockCountLargerThanContents()
    {
        QByteArray bytes = representativeFile();
        writeValue<qint32>(bytes, 4, 3);
        verifyRejected(parse(bytes));
    }

    void rejectsDataBeyondDeclaredBlockCount()
    {
        QByteArray bytes = representativeFile();
        writeValue<qint32>(bytes, 4, 1);
        verifyRejected(parse(bytes));
    }

    void rejectsTruncatedDeclaredDataPayload()
    {
        const QByteArray oneRecord =
            rideDataRecord(100, 100.0f, 120, 80, 2000, 0);
        verifyRejected(parse(fileWithRideDataBlock(
            2, rideDataRecordSize(100), oneRecord)));
    }

    void rejectsUnsupportedFileVersion()
    {
        QByteArray bytes = representativeFile();
        writeValue<qint16>(bytes, 2, 120);
        verifyRejected(parse(bytes));
    }

    void rejectsUnsupportedBlockVersion()
    {
        QByteArray bytes = representativeFile();
        writeValue<qint16>(bytes, 8 + 2, 120);
        verifyRejected(parse(bytes));
    }

    void rejectsUnsupportedCourseBlockVersion()
    {
        QByteArray bytes = representativeFile(110, 0, 0, true);
        const int courseBlockOffset =
            8 + 12 + rideInformationRecordSize(110);
        writeValue<qint16>(bytes, courseBlockOffset + 2, 110);
        verifyRejected(parse(bytes));
    }

    void rejectsUnsupportedBlockFingerprint()
    {
        QByteArray bytes = representativeFile();
        writeValue<qint16>(bytes, 8, 3990);
        verifyRejected(parse(bytes));
    }

    void rejectsRideInformationSizeForDeclaredVersion()
    {
        QByteArray bytes = representativeFile(100);
        writeValue<qint16>(bytes, 8 + 2, 110);
        verifyRejected(parse(bytes));
    }

    void rejectsRideDataSizeForDeclaredVersion()
    {
        QByteArray bytes = representativeFile(100);
        const int dataBlockOffset =
            8 + 12 + rideInformationRecordSize(100);
        writeValue<qint16>(bytes, dataBlockOffset + 2, 110);
        verifyRejected(parse(bytes));
    }

    void rejectsMissingRequiredBlocks()
    {
        verifyRejected(parse(fileHeader(100, 0)));

        QByteArray informationOnly = fileHeader(100, 1);
        appendRideInformationBlock(informationOnly, 100);
        verifyRejected(parse(informationOnly));
    }

    void rejectsZeroRecordRideInformationBlock()
    {
        verifyRejected(parse(fileWithBlock(3010, 0, 0)));
    }

    void rejectsZeroRecordRideDataBlock()
    {
        verifyRejected(parse(fileWithRideDataBlock(0, 0, QByteArray())));
    }

    void rejectsHalfEmptyOptionalBlocks()
    {
        verifyRejected(parse(fileWithBlock(2040, 0, 10)));
        verifyRejected(parse(fileWithBlock(2040, 1, 0)));
    }

    void rejectsNegativeRecordCount()
    {
        verifyRejected(parse(fileWithBlock(2040, -1, 10)));
    }

    void rejectsNegativeRecordSize()
    {
        verifyRejected(parse(fileWithBlock(2040, 1, -10)));
    }

    void rejectsOversizedBlockProduct()
    {
        const qint32 maximum = std::numeric_limits<qint32>::max();
        verifyRejected(parse(fileWithBlock(2040, maximum, maximum)));
    }

    void rejectsWrongRideInformationRecordSize_data()
    {
        QTest::addColumn<qint16>("version");
        QTest::addColumn<int>("adjustment");
        QTest::newRow("version-100-short") << qint16(100) << -1;
        QTest::newRow("version-100-long") << qint16(100) << 1;
        QTest::newRow("version-110-short") << qint16(110) << -1;
        QTest::newRow("version-110-long") << qint16(110) << 1;
    }

    void rejectsWrongRideInformationRecordSize()
    {
        QFETCH(qint16, version);
        QFETCH(int, adjustment);
        const int size = rideInformationRecordSize(version) + adjustment;
        verifyRejected(parse(fileWithBlock(
            3010, 1, size, QByteArray(size, '\0'), version)));
    }

    void rejectsWrongRideDataRecordSize_data()
    {
        QTest::addColumn<qint16>("version");
        QTest::addColumn<int>("adjustment");
        QTest::newRow("version-100-short") << qint16(100) << -1;
        QTest::newRow("version-100-long") << qint16(100) << 1;
        QTest::newRow("version-110-short") << qint16(110) << -1;
        QTest::newRow("version-110-long") << qint16(110) << 1;
    }

    void rejectsWrongRideDataRecordSize()
    {
        QFETCH(qint16, version);
        QFETCH(int, adjustment);
        const int size = rideDataRecordSize(version) + adjustment;
        verifyRejected(parse(fileWithRideDataBlock(
            1, size, QByteArray(size, '\0'), version)));
    }
};

QTEST_APPLESS_MAIN(TestTacxCafBounds)
#include "testTacxCafBounds.moc"
