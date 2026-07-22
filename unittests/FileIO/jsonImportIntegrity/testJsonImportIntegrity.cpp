/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

#include "TestJsonImportIntegrity.h"
#include "JsonRideFile.h"
#include "RideFile.h"

#include <QFile>
#include <QTemporaryFile>

#include <memory>

namespace {

    static QByteArray validDocument()
    {
        return QByteArray(R"JSON({
"RIDE": {
  "STARTTIME": "2026/01/02 03:04:05 UTC",
  "RECINTSECS": 1,
  "DEVICETYPE": "Integrity test",
  "IDENTIFIER": "ride-id",
  "OVERRIDES": [
    {"Average Power": {"value": "200", "units": "watts"}}
  ],
  "TAGS": {"Sport": "Bike", "Notes": "complete"},
  "INTERVALS": [
    {"NAME": "work", "START": 0, "STOP": 60,
     "COLOR": "#ff0000", "PTEST": "true"}
  ],
  "CALIBRATIONS": [
    {"NAME": "zero", "START": 10, "VALUE": 123}
  ],
  "REFERENCES": [
    {"WATTS": 200}
  ],
  "XDATA": [
    {"NAME": "aux", "VALUES": ["value"], "UNITS": ["unit"],
     "SAMPLES": [{"SECS": 0, "KM": 0, "VALUES": [1]}]}
  ],
  "SAMPLES": [
    {"SECS": 0, "KM": 0, "WATTS": 200, "HR": 120,
     "INTERVAL": 1}
  ]
}
})JSON");
    }

    static RideFile *importDocument(const QByteArray &contents,
                                    QStringList &errors)
    {
        QTemporaryFile temporary;
        if (!temporary.open())
            qFatal("Could not create JSON import test file");
        if (temporary.write(contents) != contents.size())
            qFatal("Could not write JSON import test file");
        if (!temporary.flush())
            qFatal("Could not flush JSON import test file");

        const QString path = temporary.fileName();
        temporary.close();
        QFile input(path);
        JsonFileReader reader;
        return reader.openRideFile(input, errors);
    }
} // namespace

void TestJsonImportIntegrity::importsCompleteDocument()
{
    QStringList errors;
    std::unique_ptr<RideFile> ride(
        importDocument(validDocument(), errors));

    QVERIFY2(ride != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
    QVERIFY(errors.isEmpty());
    QCOMPARE(ride->dataPoints().size(), 1);
}

void TestJsonImportIntegrity::acceptsValidDocumentWithExistingDiagnostics()
{
    const QString diagnostic = QStringLiteral("pre-existing diagnostic");
    QStringList errors{diagnostic};
    std::unique_ptr<RideFile> ride(
        importDocument(validDocument(), errors));

    QVERIFY(ride != nullptr);
    QCOMPARE(errors, QStringList{diagnostic});
}

void TestJsonImportIntegrity::rejectsMalformedDocuments_data()
{
    QTest::addColumn<QByteArray>("contents");

    const QByteArray valid = validDocument();
    const QByteArray structuralCharacters =
        QByteArrayLiteral("{}[]") + QByteArrayLiteral(",:");
    int boundary = 0;
    for (int index = 0; index < valid.size() - 1; ++index) {
        if (!structuralCharacters.contains(valid.at(index)))
            continue;
        const QByteArray rowName = QByteArray("boundary-")
            + QByteArray::number(++boundary);
        QTest::newRow(rowName.constData()) << valid.left(index + 1);
    }

    const QList<QByteArray> sections{
        QByteArrayLiteral("\"STARTTIME\""),
        QByteArrayLiteral("\"OVERRIDES\""),
        QByteArrayLiteral("\"TAGS\""),
        QByteArrayLiteral("\"INTERVALS\""),
        QByteArrayLiteral("\"CALIBRATIONS\""),
        QByteArrayLiteral("\"REFERENCES\""),
        QByteArrayLiteral("\"XDATA\""),
        QByteArrayLiteral("\"SAMPLES\"")
    };
    for (const QByteArray &section : sections) {
        const int position = valid.indexOf(section);
        QVERIFY(position >= 0);
        const QByteArray label = section.mid(1, section.size() - 2)
            .toLower();
        const QByteArray rowName = QByteArray("before-") + label;
        QTest::newRow(rowName.constData()) << valid.left(position);
    }

    QTest::newRow("empty") << QByteArray();
    QTest::newRow("trailing-comma") << valid + ',';
    QTest::newRow("trailing-object") << valid + "{}";
    QTest::newRow("trailing-token") << valid + "invalid";

    QByteArray mismatchedRoot = valid;
    mismatchedRoot.chop(1);
    mismatchedRoot.append(']');
    QTest::newRow("mismatched-root") << mismatchedRoot;

    QByteArray invalidUtf8 = QByteArray::fromHex("efbbbf") + valid;
    invalidUtf8.append(char(0xff));
    QTest::newRow("invalid-utf8-with-bom") << invalidUtf8;
}

void TestJsonImportIntegrity::rejectsMalformedDocuments()
{
    QFETCH(QByteArray, contents);
    QStringList errors;
    std::unique_ptr<RideFile> ride(importDocument(contents, errors));

    QVERIFY2(ride == nullptr,
             "Malformed JSON returned a partial activity");
    QVERIFY2(!errors.isEmpty(),
             "Malformed JSON produced no import error");
}

QTEST_GUILESS_MAIN(TestJsonImportIntegrity)
