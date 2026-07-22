/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

#include "FitlogRideFile.h"
#include "GpxRideFile.h"
#include "RideFile.h"
#include "TcxRideFile.h"

#include <QTemporaryFile>

class TestXmlImportIntegrity : public QObject
{
    Q_OBJECT

    enum Format {
        Tcx,
        Gpx,
        Fitlog
    };

    struct ImportResult {
        RideFile *ride = nullptr;
        QList<RideFile*> rides;
        QStringList errors;
    };

    static QByteArray timestamp(int seconds = 0)
    {
        return QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5),
                         QTimeZone::UTC)
            .addSecs(seconds)
            .toString(Qt::ISODate)
            .toLatin1();
    }

    static QByteArray tcxActivity(int seconds = 0)
    {
        const QByteArray time = timestamp(seconds);
        return "<Activity Sport=\"Biking\"><Id>" + time
            + "</Id><Lap StartTime=\"" + time
            + "\"><TotalTimeSeconds>1</TotalTimeSeconds>"
              "<DistanceMeters>10</DistanceMeters><Track><Trackpoint><Time>"
            + time
            + "</Time><DistanceMeters>10</DistanceMeters>"
              "<HeartRateBpm><Value>120</Value></HeartRateBpm>"
              "</Trackpoint></Track></Lap></Activity>";
    }

    static QByteArray tcxDocument(const QByteArray &activities)
    {
        return "<?xml version=\"1.0\"?><TrainingCenterDatabase><Activities>"
            + activities
            + "</Activities></TrainingCenterDatabase>";
    }

    static QByteArray gpxDocument()
    {
        return "<?xml version=\"1.0\"?><gpx><trk><trkseg>"
               "<trkpt lat=\"60.1\" lon=\"24.9\"><ele>12</ele><time>"
            + timestamp()
            + "</time><extensions><heartrate>121</heartrate></extensions>"
              "</trkpt></trkseg></trk></gpx>";
    }

    static QByteArray fitlogActivity(int seconds = 0)
    {
        const QByteArray time = timestamp(seconds);
        return "<Activity StartTime=\"" + time
            + "\"><Metadata Source=\"IntegrityTest\"/><Track StartTime=\""
            + time
            + "\"><pt tm=\"0\" dist=\"0\" ele=\"12\" hr=\"122\" "
              "cadence=\"80\" power=\"180\" lat=\"60.1\" lon=\"24.9\"/>"
              "</Track></Activity>";
    }

    static QByteArray fitlogDocument(const QByteArray &activities)
    {
        return "<?xml version=\"1.0\"?><FitnessWorkbook><AthleteLog>"
            + activities
            + "</AthleteLog></FitnessWorkbook>";
    }

    static QByteArray validDocument(Format format)
    {
        switch (format) {
        case Tcx:
            return tcxDocument(tcxActivity());
        case Gpx:
            return gpxDocument();
        case Fitlog:
            return fitlogDocument(fitlogActivity());
        }
        return QByteArray();
    }

    static RideFile *importDocument(Format format, const QByteArray &contents,
                                    QStringList &errors,
                                    QList<RideFile*> *rides)
    {
        QTemporaryFile temporary;
        if (!temporary.open())
            qFatal("Could not create XML import test file");
        if (temporary.write(contents) != contents.size())
            qFatal("Could not write XML import test file");
        if (!temporary.flush())
            qFatal("Could not flush XML import test file");

        if (format == Tcx) {
            temporary.close();
            TcxFileReader reader;
            return reader.openRideFile(temporary, errors, rides);
        }

        if (!temporary.seek(0))
            qFatal("Could not rewind XML import test file");
        if (format == Gpx) {
            GpxFileReader reader;
            return reader.openRideFile(temporary, errors, rides);
        }

        FitlogFileReader reader;
        return reader.openRideFile(temporary, errors, rides);
    }

    static ImportResult importDocument(Format format,
                                       const QByteArray &contents,
                                       bool collectRides = true)
    {
        ImportResult result;
        result.ride = importDocument(
            format, contents, result.errors,
            collectRides ? &result.rides : nullptr);
        return result;
    }

    static void destroyResult(ImportResult &result)
    {
        if (result.ride && !result.rides.contains(result.ride))
            delete result.ride;
        qDeleteAll(result.rides);
        result.rides.clear();
        result.ride = nullptr;
    }

    static void addMalformedRows(Format format, const char *formatName)
    {
        const QByteArray valid = validDocument(format);
        int boundary = 0;
        for (int index = 0; index < valid.size() - 1; ++index) {
            if (valid.at(index) != '>')
                continue;
            const QByteArray rowName = QByteArray(formatName)
                + "-boundary-" + QByteArray::number(++boundary);
            QTest::newRow(rowName.constData())
                << int(format) << valid.left(index + 1);
        }

        QTest::newRow((QByteArray(formatName) + "-trailing-open").constData())
            << int(format) << valid + "<";
        QTest::newRow((QByteArray(formatName) + "-trailing-element").constData())
            << int(format) << valid + "<unexpected/>";

        QByteArray mismatched = valid;
        const int closingTag = mismatched.lastIndexOf("</");
        mismatched.replace(closingTag, mismatched.size() - closingTag,
                           "</Mismatched>");
        QTest::newRow((QByteArray(formatName) + "-mismatched-root").constData())
            << int(format) << mismatched;
    }

private slots:
    void importsValidDocuments_data()
    {
        QTest::addColumn<int>("formatValue");
        QTest::newRow("tcx") << int(Tcx);
        QTest::newRow("gpx") << int(Gpx);
        QTest::newRow("fitlog") << int(Fitlog);
    }

    void importsValidDocuments()
    {
        QFETCH(int, formatValue);
        const Format format = Format(formatValue);
        ImportResult result = importDocument(format, validDocument(format));

        const bool imported = result.ride != nullptr;
        const int points = imported ? result.ride->dataPoints().size() : 0;
        const QString message = result.errors.join(QLatin1Char('\n'));
        destroyResult(result);

        QVERIFY2(imported, qPrintable(message));
        QCOMPARE(points, 1);
    }

    void rejectsMalformedDocuments_data()
    {
        QTest::addColumn<int>("formatValue");
        QTest::addColumn<QByteArray>("contents");
        addMalformedRows(Tcx, "tcx");
        addMalformedRows(Gpx, "gpx");
        addMalformedRows(Fitlog, "fitlog");
    }

    void rejectsMalformedDocuments()
    {
        QFETCH(int, formatValue);
        QFETCH(QByteArray, contents);
        ImportResult result = importDocument(Format(formatValue), contents);

        const bool rejected = result.ride == nullptr;
        const int published = result.rides.size();
        const QString message = result.errors.join(QLatin1Char('\n'));
        destroyResult(result);

        QVERIFY2(rejected, "Malformed XML returned a partial activity");
        QCOMPARE(published, 0);
        QVERIFY2(!message.isEmpty(), "Malformed XML produced no import error");
    }

    void malformedMultiActivityDoesNotMutateCallerList_data()
    {
        QTest::addColumn<int>("formatValue");
        QTest::addColumn<QByteArray>("contents");

        const QByteArray tcx = tcxDocument(
            tcxActivity() + tcxActivity(60));
        QTest::newRow("tcx") << int(Tcx)
                              << tcx.left(tcx.lastIndexOf("</Activity>") + 5);

        const QByteArray fitlog = fitlogDocument(
            fitlogActivity() + fitlogActivity(60));
        QTest::newRow("fitlog")
            << int(Fitlog)
            << fitlog.left(fitlog.lastIndexOf("</Activity>") + 5);
    }

    void malformedMultiActivityDoesNotMutateCallerList()
    {
        QFETCH(int, formatValue);
        QFETCH(QByteArray, contents);

        RideFile *sentinel = new RideFile();
        QList<RideFile*> rides{sentinel};
        QStringList errors;
        RideFile *result = importDocument(
            Format(formatValue), contents, errors, &rides);

        const bool rejected = result == nullptr;
        const bool unchanged = rides.size() == 1
            && rides.constFirst() == sentinel;
        const QString message = errors.join(QLatin1Char('\n'));

        if (result && result != sentinel && !rides.contains(result))
            delete result;
        for (RideFile *ride : rides) {
            if (ride != sentinel)
                delete ride;
        }
        delete sentinel;

        QVERIFY2(rejected, "Malformed multi-activity XML was accepted");
        QVERIFY2(unchanged, "Malformed import published partial activities");
        QVERIFY2(!message.isEmpty(), "Malformed XML produced no import error");
    }

    void preservesValidMultiActivitySemantics_data()
    {
        QTest::addColumn<int>("formatValue");
        QTest::addColumn<QByteArray>("contents");
        QTest::newRow("tcx")
            << int(Tcx)
            << tcxDocument(tcxActivity() + tcxActivity(60));
        QTest::newRow("fitlog")
            << int(Fitlog)
            << fitlogDocument(fitlogActivity() + fitlogActivity(60));
    }

    void preservesValidMultiActivitySemantics()
    {
        QFETCH(int, formatValue);
        QFETCH(QByteArray, contents);
        ImportResult result = importDocument(Format(formatValue), contents);

        const bool imported = result.ride != nullptr;
        const int rides = result.rides.size();
        const bool primaryPublished = imported && !result.rides.isEmpty()
            && result.rides.constFirst() == result.ride;
        const QString message = result.errors.join(QLatin1Char('\n'));
        destroyResult(result);

        QVERIFY2(imported, qPrintable(message));
        QCOMPARE(rides, 2);
        QVERIFY(primaryPublished);
    }

    void discardsUnrequestedExtraActivities_data()
    {
        preservesValidMultiActivitySemantics_data();
    }

    void discardsUnrequestedExtraActivities()
    {
        QFETCH(int, formatValue);
        QFETCH(QByteArray, contents);
        ImportResult result = importDocument(
            Format(formatValue), contents, false);

        const bool imported = result.ride != nullptr;
        const int points = imported ? result.ride->dataPoints().size() : 0;
        const QString message = result.errors.join(QLatin1Char('\n'));
        destroyResult(result);

        QVERIFY2(imported, qPrintable(message));
        QCOMPARE(points, 1);
    }
};

QTEST_GUILESS_MAIN(TestXmlImportIntegrity)

#include "testXmlImportIntegrity.moc"
