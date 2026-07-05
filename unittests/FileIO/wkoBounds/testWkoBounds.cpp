/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "WkoTestStubs.h"

#include <QScopedPointer>
#include <QTemporaryFile>
#include <QTest>

#include <limits>

class TestWkoBounds : public QObject
{
    Q_OBJECT

    struct ParseResult {
        bool imported;
        QStringList errors;
    };

    static void appendU16(QByteArray &bytes, quint16 value)
    {
        bytes.append(static_cast<char>(value & 0xff));
        bytes.append(static_cast<char>((value >> 8) & 0xff));
    }

    static void appendU32(QByteArray &bytes, quint32 value)
    {
        appendU16(bytes, static_cast<quint16>(value & 0xffff));
        appendU16(bytes, static_cast<quint16>(value >> 16));
    }

    static void appendText(QByteArray &bytes, const QByteArray &text)
    {
        if (text.size() < 255) {
            bytes.append(static_cast<char>(text.size()));
        } else {
            bytes.append(static_cast<char>(0xff));
            appendU16(bytes, static_cast<quint16>(text.size()));
        }
        bytes.append(text);
    }

    static void appendEncodedText(QByteArray &bytes, quint16 declaredLength,
                                  int payloadLength, char payload = 'P')
    {
        if (declaredLength < 255) {
            bytes.append(static_cast<char>(declaredLength));
        } else {
            bytes.append(static_cast<char>(0xff));
            appendU16(bytes, declaredLength);
        }
        bytes.append(QByteArray(payloadLength, payload));
    }

    static void padTo(QByteArray &bytes, int size)
    {
        if (bytes.size() < size)
            bytes.append(QByteArray(size - bytes.size(), '\0'));
    }

    static QByteArray graphFile(quint16 declaredLength, int payloadLength)
    {
        QByteArray bytes("WKO\x1a", 4);
        appendU32(bytes, 28);
        appendU32(bytes, 0);
        appendText(bytes, QByteArray());
        appendText(bytes, QByteArray());
        appendEncodedText(bytes, declaredLength, payloadLength);
        padTo(bytes, qMax(1024, bytes.size()));
        return bytes;
    }

    static QByteArray truncatedGraphFile()
    {
        QByteArray bytes("WKO\x1a", 4);
        appendU32(bytes, 28);
        appendU32(bytes, 0);
        appendText(bytes, QByteArray(486, 'G'));
        appendText(bytes, QByteArray());
        appendEncodedText(bytes, 31, 9);
        Q_ASSERT(bytes.size() == 512);
        return bytes;
    }

    static QByteArray truncatedGraphLengthFile()
    {
        QByteArray bytes("WKO\x1a", 4);
        appendU32(bytes, 28);
        appendU32(bytes, 0);
        appendText(bytes, QByteArray(495, 'G'));
        appendText(bytes, QByteArray());
        bytes.append(static_cast<char>(0xff));
        Q_ASSERT(bytes.size() == 512);
        return bytes;
    }

    static QByteArray chartPrefix()
    {
        QByteArray bytes("WKO\x1a", 4);
        appendU32(bytes, 28);
        appendU32(bytes, 0);
        appendText(bytes, QByteArray());
        appendText(bytes, QByteArray());
        appendText(bytes, QByteArray("P"));
        appendU32(bytes, 2);
        appendText(bytes, QByteArray());
        appendU32(bytes, 0);
        appendText(bytes, QByteArray());
        appendText(bytes, QByteArray());
        appendU32(bytes, 0);

        for (int i = 0; i < 5; ++i) appendU32(bytes, 0);
        bytes.append(QByteArray(8, '\0'));
        appendU32(bytes, 0);
        bytes.append(QByteArray(28, '\0'));

        appendU32(bytes, 0);
        appendU32(bytes, 1);
        appendU32(bytes, 0);

        for (int i = 0; i < 16; ++i) {
            bytes.append(QByteArray(44, '\0'));
            appendU16(bytes, 0);
        }

        appendU16(bytes, 0);
        appendU16(bytes, 0);
        appendU16(bytes, 1);
        appendU32(bytes, 0x01ffff);
        return bytes;
    }

    static QByteArray chartFile(quint16 declaredLength, int payloadLength)
    {
        QByteArray bytes = chartPrefix();
        appendU16(bytes, declaredLength);
        bytes.append(QByteArray(payloadLength, 'C'));
        appendU32(bytes, 0);
        return bytes;
    }

    static QByteArray validChartFile()
    {
        QByteArray bytes = chartPrefix();
        const QByteArray name("CRideSettingsConfig");
        appendU16(bytes, static_cast<quint16>(name.size()));
        bytes.append(name);
        appendU32(bytes, 2);
        appendText(bytes, QByteArray());
        appendU32(bytes, 0);
        appendU16(bytes, 1);
        appendU16(bytes, 0);
        bytes.append(static_cast<char>(0x01));
        bytes.append(static_cast<char>(0x00));
        return bytes;
    }

    static QByteArray validRideFile()
    {
        QByteArray bytes = chartPrefix();
        bytes.chop(6);
        appendU16(bytes, 0);

        appendU16(bytes, 1);
        appendU16(bytes, 0);
        bytes.append(static_cast<char>(0x01));
        bytes.append(static_cast<char>(0x00));
        return bytes;
    }

    static ParseResult parse(const QByteArray &contents)
    {
        QTemporaryFile temporary;
        if (!temporary.open()
            || temporary.write(contents) != contents.size()
            || !temporary.flush()) {
            return { false, QStringList() << "Could not create test file" };
        }

        QFile input(temporary.fileName());
        QStringList errors;
        WkoFileReader reader;
        QScopedPointer<RideFile> ride(reader.openRideFile(input, errors));
        return { !ride.isNull(), errors };
    }

    static ParseResult parseSizedFile(qint64 size)
    {
        QTemporaryFile temporary;
        if (!temporary.open() || !temporary.resize(size)) {
            return { false, QStringList() << "Could not create sized test file" };
        }
        temporary.close();

        QFile input(temporary.fileName());
        QStringList errors;
        WkoFileReader reader;
        QScopedPointer<RideFile> ride(reader.openRideFile(input, errors));
        return { !ride.isNull(), errors };
    }

    static bool hasError(const ParseResult &result, const QString &message)
    {
        return result.errors.contains(message);
    }

private slots:
    void acceptsMaximumGraphName()
    {
        const ParseResult result = parse(graphFile(31, 31));
        QVERIFY(!hasError(result, "WKO graph name exceeds 31 bytes"));
        QVERIFY(!hasError(result, "Truncated WKO graph name"));
    }

    void rejectsOversizedGraphName_data()
    {
        QTest::addColumn<int>("length");
        QTest::newRow("length-32") << 32;
        QTest::newRow("length-254") << 254;
        QTest::newRow("length-65535") << 65535;
    }

    void rejectsOversizedGraphName()
    {
        QFETCH(int, length);
        const ParseResult result = parse(graphFile(
            static_cast<quint16>(length), length));
        QVERIFY(!result.imported);
        QVERIFY2(hasError(result, "WKO graph name exceeds 31 bytes"),
                 qPrintable(result.errors.join('\n')));
    }

    void rejectsTruncatedGraphName()
    {
        const ParseResult result = parse(truncatedGraphFile());
        QVERIFY(!result.imported);
        QVERIFY2(hasError(result, "Truncated WKO graph name"),
                 qPrintable(result.errors.join('\n')));
    }

    void rejectsTruncatedGraphNameLength()
    {
        const ParseResult result = parse(truncatedGraphLengthFile());
        QVERIFY(!result.imported);
        QVERIFY2(hasError(result, "Truncated WKO graph name"),
                 qPrintable(result.errors.join('\n')));
    }

    void acceptsMaximumChartName()
    {
        const ParseResult result = parse(chartFile(31, 31));
        QVERIFY(!hasError(result, "WKO chart name exceeds 31 bytes"));
        QVERIFY(!hasError(result, "Truncated WKO chart name"));
    }

    void acceptsKnownChartName()
    {
        const ParseResult result = parse(validChartFile());
        QVERIFY2(result.imported, qPrintable(result.errors.join('\n')));
        QVERIFY(!hasError(result, "WKO chart name exceeds 31 bytes"));
        QVERIFY(!hasError(result, "Truncated WKO chart name"));
    }

    void rejectsOversizedChartName_data()
    {
        QTest::addColumn<int>("length");
        QTest::newRow("length-32") << 32;
        QTest::newRow("length-254") << 254;
        QTest::newRow("length-65535") << 65535;
    }

    void rejectsOversizedChartName()
    {
        QFETCH(int, length);
        const ParseResult result = parse(chartFile(
            static_cast<quint16>(length), length));
        QVERIFY(!result.imported);
        QVERIFY2(hasError(result, "WKO chart name exceeds 31 bytes"),
                 qPrintable(result.errors.join('\n')));
    }

    void rejectsTruncatedChartName()
    {
        QByteArray bytes = chartPrefix();
        appendU16(bytes, 31);
        bytes.append(QByteArray(5, 'C'));

        const ParseResult result = parse(bytes);
        QVERIFY(!result.imported);
        QVERIFY2(hasError(result, "Truncated WKO chart name"),
                 qPrintable(result.errors.join('\n')));
    }

    void rejectsTruncatedChartNameLength()
    {
        QByteArray bytes = chartPrefix();
        bytes.append(static_cast<char>(31));

        const ParseResult result = parse(bytes);
        QVERIFY(!result.imported);
        QVERIFY2(hasError(result, "Truncated WKO chart name"),
                 qPrintable(result.errors.join('\n')));
    }

    void rejectsUnsupportedFileSize()
    {
        const qint64 supportedMaximum = std::numeric_limits<int>::max() / 8;
        const ParseResult result = parseSizedFile(supportedMaximum + 1);

        QVERIFY(!result.imported);
        QVERIFY2(hasError(result, "WKO+ file exceeds the supported size"),
                 qPrintable(result.errors.join('\n')));
    }

    void importsMinimalValidRide()
    {
        const ParseResult result = parse(validRideFile());
        QVERIFY2(result.imported, qPrintable(result.errors.join('\n')));
    }
};

QTEST_APPLESS_MAIN(TestWkoBounds)
#include "testWkoBounds.moc"
