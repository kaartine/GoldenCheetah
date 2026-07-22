/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

#include "TcxParser.h"
#include "TcxRideFile.h"

#include <QTemporaryFile>
#include <QTimeZone>

void setTcxPointBudgetTestSettings(int smartRecording, int highWaterMark);

class TestTcxPointBudget : public QObject
{
    Q_OBJECT

    static QString timestamp(double seconds)
    {
        const QDateTime start(QDate(2026, 7, 18), QTime(10, 0),
                              QTimeZone::UTC);
        return start.addMSecs(qRound64(seconds * 1000.0))
            .toString(Qt::ISODateWithMs);
    }

    static QString trackpoint(double seconds, int distanceMeters)
    {
        return QStringLiteral(
            "<Trackpoint><Time>%1</Time><DistanceMeters>%2"
            "</DistanceMeters></Trackpoint>")
            .arg(timestamp(seconds))
            .arg(distanceMeters);
    }

    static QString gpsTrackpoint(double seconds, int distanceMeters)
    {
        return QStringLiteral(
            "<Trackpoint><Time>%1</Time><DistanceMeters>%2"
            "</DistanceMeters><Position><LatitudeDegrees>60.0"
            "</LatitudeDegrees><LongitudeDegrees>24.0"
            "</LongitudeDegrees></Position></Trackpoint>")
            .arg(timestamp(seconds))
            .arg(distanceMeters);
    }

    static QString lap(const QString &contents, double startSeconds,
                       double totalSeconds, int distanceMeters)
    {
        return QStringLiteral(
            "<Lap StartTime=\"%1\"><TotalTimeSeconds>%2"
            "</TotalTimeSeconds>%3<DistanceMeters>%4</DistanceMeters>"
            "</Lap>")
            .arg(timestamp(startSeconds))
            .arg(QString::number(totalSeconds, 'g', 15))
            .arg(contents)
            .arg(distanceMeters);
    }

    static QString activity(const QString &sport, const QString &laps,
                            int idSeconds = 0)
    {
        return QStringLiteral(
            "<Activity Sport=\"%1\"><Id>%2</Id>%3</Activity>")
            .arg(sport, timestamp(idSeconds), laps);
    }

    static QString document(const QString &activities)
    {
        return QStringLiteral(
            "<?xml version=\"1.0\"?><TrainingCenterDatabase><Activities>"
            "%1</Activities></TrainingCenterDatabase>")
            .arg(activities);
    }

    static QString expandedSwimActivity(int pointCount, int idSeconds = 0)
    {
        Q_ASSERT(pointCount > 0);

        QString tracks = QStringLiteral("<Track>");
        int remainingPoints = pointCount - 1;
        int elapsedSeconds = 0;
        int distanceMeters = 25;
        tracks += trackpoint(elapsedSeconds, distanceMeters);
        while (remainingPoints > 0) {
            const int gap = qMin(remainingPoints, 7500);
            elapsedSeconds += gap;
            distanceMeters += 25;
            tracks += trackpoint(elapsedSeconds, distanceMeters);
            remainingPoints -= gap;
        }
        tracks += QStringLiteral("</Track>");

        return activity(QStringLiteral("Other"),
                        lap(tracks, 0, elapsedSeconds, distanceMeters),
                        idSeconds);
    }

    static RideFile *importTcx(const QString &xml, QStringList &errors,
                               QList<RideFile*> *rides = nullptr)
    {
        QTemporaryFile temporary;
        if (!temporary.open())
            qFatal("Could not create temporary TCX file");
        const QByteArray bytes = xml.toUtf8();
        if (temporary.write(bytes) != bytes.size())
            qFatal("Could not write temporary TCX file");
        temporary.close();

        QFile input(temporary.fileName());
        TcxFileReader reader;
        return reader.openRideFile(input, errors, rides);
    }

    static int pointCount(const QList<RideFile*> &rides)
    {
        int count = 0;
        for (const RideFile *ride : rides)
            count += ride->dataPoints().size();
        return count;
    }

    static void destroyResult(RideFile *result, QList<RideFile*> &rides)
    {
        if (result && !rides.contains(result))
            delete result;
        qDeleteAll(rides);
        rides.clear();
    }

    static bool hasStrictlyIncreasingTimestamps(const RideFile &ride)
    {
        const QVector<RideFilePoint*> &points = ride.dataPoints();
        for (int index = 1; index < points.size(); ++index) {
            if (points[index - 1]->secs >= points[index]->secs)
                return false;
        }
        return true;
    }

private slots:
    void init()
    {
        setTcxPointBudgetTestSettings(Qt::Checked, 25);
    }

    void importsOrdinarySwimInterpolation()
    {
        const QString tracks = QStringLiteral("<Track>")
            + trackpoint(0, 25) + trackpoint(5, 50)
            + QStringLiteral("</Track>");
        const QString xml = document(activity(
            QStringLiteral("Other"), lap(tracks, 0, 5, 50)));

        QStringList errors;
        QList<RideFile*> rides;
        RideFile *result = importTcx(xml, errors, &rides);

        QVERIFY2(result != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(rides.size(), 1);
        QCOMPARE(rides.constFirst(), result);
        QCOMPARE(result->getTag(QStringLiteral("Sport"), QString()),
                 QStringLiteral("Swim"));
        QCOMPARE(result->dataPoints().size(), 6);
        QCOMPARE(result->dataPoints().constFirst()->secs, 0.0);
        QCOMPARE(result->dataPoints().constLast()->secs, 5.0);
        destroyResult(result, rides);
    }

    void importsMultipleActivitiesWithoutLeakingUnreturnedRides()
    {
        const QString firstTrack = QStringLiteral("<Track>")
            + trackpoint(0, 0) + QStringLiteral("</Track>");
        const QString secondTrack = QStringLiteral("<Track>")
            + trackpoint(1, 0) + QStringLiteral("</Track>");
        const QString xml = document(
            activity(QStringLiteral("Biking"), lap(firstTrack, 0, 1, 0))
            + activity(QStringLiteral("Biking"),
                       lap(secondTrack, 1, 1, 0), 1));

        QStringList errors;
        RideFile *result = importTcx(xml, errors);

        QVERIFY2(result != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(result->dataPoints().size(), 1);
        delete result;
    }

    void enforcesPointBudgetAcrossIndividuallyValidActivities()
    {
        QStringList errors;
        QList<RideFile*> rides;
        RideFile *result = importTcx(
            document(expandedSwimActivity(90001)
                     + expandedSwimActivity(90001, 100000)),
            errors, &rides);

        const bool rejected = result == nullptr;
        const bool rolledBack = rides.isEmpty();
        destroyResult(result, rides);

        QVERIFY2(rejected,
                 "Two individually valid activities exceeded the file budget");
        QVERIFY2(rolledBack, "Rejected multi-activity import was published");
        QVERIFY2(errors.join(QLatin1Char('\n')).contains(
                     QStringLiteral("point"), Qt::CaseInsensitive),
                 qPrintable(errors.join(QLatin1Char('\n'))));
    }

    void acceptsExactWholeFilePointBudget()
    {
        QStringList errors;
        RideFile *result = importTcx(document(expandedSwimActivity(
                                         TcxParser::MaximumImportPoints)),
                                     errors);

        QVERIFY2(result != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(result->dataPoints().size(),
                 TcxParser::MaximumImportPoints);
        delete result;
    }

    void rejectsOnePointBeyondWholeFilePointBudget()
    {
        QStringList errors;
        QList<RideFile*> rides;
        RideFile *result = importTcx(
            document(expandedSwimActivity(
                TcxParser::MaximumImportPoints + 1)),
            errors, &rides);

        const bool rejected = result == nullptr;
        const bool rolledBack = rides.isEmpty();
        destroyResult(result, rides);

        QVERIFY(rejected);
        QVERIFY2(rolledBack, "Over-budget activity was published");
        QVERIFY2(errors.join(QLatin1Char('\n')).contains(
                     QStringLiteral("point"), Qt::CaseInsensitive),
                 qPrintable(errors.join(QLatin1Char('\n'))));
    }

    void preservesSourceEndpointAfterCappedSwimGap()
    {
        const QString tracks = QStringLiteral("<Track>")
            + trackpoint(0, 25) + trackpoint(10000, 50)
            + QStringLiteral("</Track>");

        QStringList errors;
        RideFile *result = importTcx(document(activity(
            QStringLiteral("Other"), lap(tracks, 0, 10000, 50))), errors);

        QVERIFY2(result != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(result->dataPoints().size(), 7502);
        QCOMPARE(result->dataPoints().constLast()->secs, 10000.0);
        QCOMPARE(result->dataPoints().constLast()->km, 0.05);
        QVERIFY(hasStrictlyIncreasingTimestamps(*result));
        delete result;
    }

    void preservesFractionalSourceEndpoint()
    {
        const QString tracks = QStringLiteral("<Track>")
            + trackpoint(0, 25) + trackpoint(5.5, 50)
            + QStringLiteral("</Track>");

        QStringList errors;
        RideFile *result = importTcx(document(activity(
            QStringLiteral("Other"), lap(tracks, 0, 5.5, 50))), errors);

        QVERIFY2(result != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(result->dataPoints().size(), 7);
        QCOMPARE(result->dataPoints().constLast()->secs, 5.5);
        QCOMPARE(result->dataPoints().constLast()->km, 0.05);
        QVERIFY(hasStrictlyIncreasingTimestamps(*result));
        delete result;
    }

    void preservesSourceEndpointAfterCappedSwimPause()
    {
        const QString activeTrack = QStringLiteral("<Track>")
            + trackpoint(0, 25) + QStringLiteral("</Track>");
        const QString laps = lap(activeTrack, 0, 0, 25)
            + lap(QString(), 0, 10000, 0);

        QStringList errors;
        RideFile *result = importTcx(document(activity(
            QStringLiteral("Other"), laps)), errors);

        QVERIFY2(result != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(result->dataPoints().size(), 7502);
        QCOMPARE(result->dataPoints().constLast()->secs, 10000.0);
        QVERIFY(hasStrictlyIncreasingTimestamps(*result));
        delete result;
    }

    void usesLapTimelineForConsecutiveSwimPauses()
    {
        const QString activeTrack = QStringLiteral("<Track>")
            + trackpoint(0, 25) + QStringLiteral("</Track>");
        const QString laps = lap(activeTrack, 0, 0, 25)
            + lap(QString(), 0, 3, 0)
            + lap(QString(), 3, 3, 0);

        QStringList errors;
        RideFile *result = importTcx(document(activity(
            QStringLiteral("Other"), laps)), errors);

        QVERIFY2(result != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(result->dataPoints().size(), 7);
        QCOMPARE(result->dataPoints().constLast()->secs, 6.0);
        QVERIFY(hasStrictlyIncreasingTimestamps(*result));
        delete result;
    }

    void ignoresNegativeSwimPausesWithoutRegressingTheCursor()
    {
        const QString initialTrack = QStringLiteral("<Track>")
            + trackpoint(0, 25) + QStringLiteral("</Track>");
        QString laps = lap(initialTrack, 0, 0, 25);
        for (int index = 1; index <= 512; ++index) {
            laps += lap(QString(), index - 1, -100000, 0);
            const QString activeTrack = QStringLiteral("<Track>")
                + trackpoint(index, (index + 1) * 25)
                + QStringLiteral("</Track>");
            laps += lap(activeTrack, index, 1, (index + 1) * 25);
        }

        QStringList errors;
        RideFile *result = importTcx(document(activity(
            QStringLiteral("Other"), laps)), errors);

        QVERIFY2(result != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(result->dataPoints().size(), 513);
        QVERIFY(hasStrictlyIncreasingTimestamps(*result));
        XDataSeries *swim = result->xdata(QStringLiteral("SWIM"));
        QVERIFY(swim != nullptr);
        for (const XDataPoint *point : swim->datapoints)
            QVERIFY(point->number[1] >= 0.0);
        delete result;
    }

    void normalizesCorruptHighWaterMark()
    {
        setTcxPointBudgetTestSettings(Qt::Checked, -1);
        const QString tracks = QStringLiteral("<Track>")
            + trackpoint(0, 25) + trackpoint(5, 50)
            + QStringLiteral("</Track>");

        QStringList errors;
        RideFile *result = importTcx(document(activity(
            QStringLiteral("Other"), lap(tracks, 0, 5, 50))), errors);

        QVERIFY2(result != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(result->dataPoints().size(), 6);
        QCOMPARE(result->dataPoints().constLast()->secs, 5.0);
        delete result;
    }

    void smartRecordingDisabledKeepsSourceSwimPoints()
    {
        setTcxPointBudgetTestSettings(Qt::Unchecked, 25);
        const QString tracks = QStringLiteral("<Track>")
            + trackpoint(0, 25) + trackpoint(5, 50)
            + QStringLiteral("</Track>");

        QStringList errors;
        RideFile *result = importTcx(document(activity(
            QStringLiteral("Other"), lap(tracks, 0, 5, 50))), errors);

        QVERIFY2(result != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(result->dataPoints().size(), 2);
        QCOMPARE(result->dataPoints().constLast()->secs, 5.0);
        QVERIFY(result->xdata(QStringLiteral("SWIM")) != nullptr);
        delete result;
    }

    void gpsOtherActivityKeepsLongGapSparse()
    {
        const QString tracks = QStringLiteral("<Track>")
            + gpsTrackpoint(0, 0) + gpsTrackpoint(10000, 1000)
            + QStringLiteral("</Track>");

        QStringList errors;
        RideFile *result = importTcx(document(activity(
            QStringLiteral("Other"), lap(tracks, 0, 10000, 1000))), errors);

        QVERIFY2(result != nullptr, qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(result->dataPoints().size(), 2);
        QCOMPARE(result->dataPoints().constLast()->secs, 10000.0);
        QVERIFY(result->xdata(QStringLiteral("SWIM")) == nullptr);
        delete result;
    }

    void rejectsRepeatedSwimGapAmplificationAndRollsBackAllActivities()
    {
        const QString firstTrack = QStringLiteral("<Track>")
            + trackpoint(0, 0) + QStringLiteral("</Track>");
        const QString firstActivity = activity(
            QStringLiteral("Biking"), lap(firstTrack, 0, 1, 0));

        QString hostileTracks = QStringLiteral("<Track>");
        for (int index = 0; index <= 24; ++index)
            hostileTracks += trackpoint(index * 7500, (index + 1) * 25);
        hostileTracks += QStringLiteral("</Track>");
        const QString hostileActivity = activity(
            QStringLiteral("Other"),
            lap(hostileTracks, 0, 24 * 7500, 25 * 25), 1);

        QStringList errors;
        QList<RideFile*> rides;
        RideFile *result = importTcx(
            document(firstActivity + hostileActivity), errors, &rides);

        const bool rejected = result == nullptr;
        const bool rolledBack = rides.isEmpty();
        const int producedPoints = pointCount(rides);
        destroyResult(result, rides);

        QVERIFY2(rejected,
                 qPrintable(QStringLiteral("Import produced %1 points")
                                .arg(producedPoints)));
        QVERIFY2(rolledBack, "Rejected multi-activity import was published");
        QVERIFY2(errors.join(QLatin1Char('\n')).contains(
                     QStringLiteral("point"), Qt::CaseInsensitive),
                 qPrintable(errors.join(QLatin1Char('\n'))));
    }

    void rejectsRepeatedSwimPauseAmplification()
    {
        const QString activeTrack = QStringLiteral("<Track>")
            + trackpoint(0, 25) + QStringLiteral("</Track>");
        QString laps = lap(activeTrack, 0, 1, 25);
        for (int index = 0; index < 24; ++index)
            laps += lap(QString(), index * 7500, 7500, 0);

        QStringList errors;
        RideFile *result = importTcx(document(activity(
            QStringLiteral("Other"), laps)), errors);

        const bool rejected = result == nullptr;
        const int producedPoints = result ? result->dataPoints().size() : 0;
        delete result;

        QVERIFY2(rejected,
                 qPrintable(QStringLiteral("Import produced %1 points")
                                .arg(producedPoints)));
        QVERIFY2(errors.join(QLatin1Char('\n')).contains(
                     QStringLiteral("point"), Qt::CaseInsensitive),
                 qPrintable(errors.join(QLatin1Char('\n'))));
    }
};

QTEST_GUILESS_MAIN(TestTcxPointBudget)

#include "testTcxPointBudget.moc"
