/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "CpCsvImport.h"

#include <QTest>

class TestCpCsvImport : public QObject
{
    Q_OBJECT

    static CpCsvImport::Builder buildRepresentativeCurve()
    {
        CpCsvImport::Builder builder;
        QString error;
        if (!builder.addRow(QStringLiteral("1,1000,2026-07-18"), 2, error)
            || !builder.addRow(QStringLiteral("5,800,2026-07-18"), 3, error)
            || !builder.addRow(QStringLiteral("10,600,2026-07-18"),
                               4, error)
            || !builder.finalize(error)) {
            qFatal("Could not build representative CP curve: %s",
                   qPrintable(error));
        }
        return builder;
    }

private slots:
    void buildsAscendingBoundedPowerSegments()
    {
        const CpCsvImport::Builder builder = buildRepresentativeCurve();
        const QVector<CpCsvImport::Segment> segments = builder.segments();

        QCOMPARE(builder.pointCount(), 10);
        QCOMPARE(segments.size(), 3);
        QCOMPARE(segments.at(0).firstSecond, 1);
        QCOMPARE(segments.at(0).lastSecond, 1);
        QCOMPARE(segments.at(0).watts, 1000.0);
        QCOMPARE(segments.at(1).firstSecond, 2);
        QCOMPARE(segments.at(1).lastSecond, 5);
        QCOMPARE(segments.at(1).watts, 750.0);
        QCOMPARE(segments.at(2).firstSecond, 6);
        QCOMPARE(segments.at(2).lastSecond, 10);
        QCOMPARE(segments.at(2).watts, 400.0);

        QVector<int> seconds;
        QVector<double> powers;
        for (const CpCsvImport::Segment &segment : segments) {
            for (int second = segment.firstSecond;
                 second <= segment.lastSecond; ++second) {
                seconds.append(second);
                powers.append(segment.watts);
            }
        }

        QCOMPARE(seconds.size(), 10);
        for (int i = 0; i < seconds.size(); ++i)
            QCOMPARE(seconds.at(i), i + 1);

        double accumulated = 0.0;
        for (int i = 0; i < powers.size(); ++i) {
            accumulated += powers.at(i);
            if (i == 0)
                QCOMPARE(accumulated / double(i + 1), 1000.0);
            else if (i == 4)
                QCOMPARE(accumulated / double(i + 1), 800.0);
            else if (i == 9)
                QCOMPARE(accumulated / double(i + 1), 600.0);
        }
    }

    void acceptsOfficialModelColumn()
    {
        CpCsvImport::Builder builder;
        QString error;

        QVERIFY2(builder.addRow(
                     QStringLiteral("1,1000,995.5,2026-07-18"), 2, error),
                 qPrintable(error));
        QVERIFY2(builder.addRow(
                     QStringLiteral("2,900,890.5,2026-07-18"), 3, error),
                 qPrintable(error));
        QVERIFY2(builder.finalize(error), qPrintable(error));
        QCOMPARE(builder.pointCount(), 2);
    }

    void rejectsUnsafeTimestamp_data()
    {
        QTest::addColumn<QString>("row");

        QTest::newRow("billion-seconds") << QStringLiteral("1000000000,200");
        QTest::newRow("negative") << QStringLiteral("-1,200");
        QTest::newRow("zero") << QStringLiteral("0,200");
        QTest::newRow("nan") << QStringLiteral("nan,200");
        QTest::newRow("positive-infinity") << QStringLiteral("inf,200");
        QTest::newRow("negative-infinity") << QStringLiteral("-inf,200");
        QTest::newRow("fractional") << QStringLiteral("1.5,200");
        QTest::newRow("not-a-number") << QStringLiteral("later,200");
    }

    void rejectsUnsafeTimestamp()
    {
        QFETCH(QString, row);
        CpCsvImport::Builder builder;
        QString error;

        QVERIFY(!builder.addRow(row, 2, error));
        QVERIFY2(!error.isEmpty(), "Rejected timestamp had no diagnostic");
        QCOMPARE(builder.pointCount(), 0);
        QVERIFY(builder.segments().isEmpty());
    }

    void normalizesNonMonotonicTimestamps()
    {
        CpCsvImport::Builder builder;
        QString error;

        QVERIFY(builder.addRow(QStringLiteral("5,800"), 2, error));
        QVERIFY(builder.addRow(QStringLiteral("1,1000"), 3, error));
        QVERIFY(builder.addRow(QStringLiteral("10,600"), 4, error));
        QVERIFY2(builder.finalize(error), qPrintable(error));

        const QVector<CpCsvImport::Segment> segments = builder.segments();
        QCOMPARE(segments.size(), 3);
        QCOMPARE(segments.at(0).firstSecond, 1);
        QCOMPARE(segments.at(0).lastSecond, 1);
        QCOMPARE(segments.at(0).watts, 1000.0);
        QCOMPARE(segments.at(1).firstSecond, 2);
        QCOMPARE(segments.at(1).lastSecond, 5);
        QCOMPARE(segments.at(1).watts, 750.0);
        QCOMPARE(segments.at(2).firstSecond, 6);
        QCOMPARE(segments.at(2).lastSecond, 10);
        QCOMPARE(segments.at(2).watts, 400.0);
    }

    void coalescesMatchingDuplicateTimestamps()
    {
        CpCsvImport::Builder builder;
        QString error;

        QVERIFY(builder.addRow(QStringLiteral("1,100"), 2, error));
        QVERIFY(builder.addRow(QStringLiteral("5,90"), 3, error));
        QVERIFY(builder.addRow(QStringLiteral("5,90"), 4, error));
        QVERIFY(builder.addRow(QStringLiteral("10,80"), 5, error));
        QVERIFY2(builder.finalize(error), qPrintable(error));
        QCOMPARE(builder.segments().size(), 3);
        QCOMPARE(builder.pointCount(), 10);
    }

    void rejectsUnsafePower_data()
    {
        QTest::addColumn<QString>("row");

        QTest::newRow("negative") << QStringLiteral("1,-1");
        QTest::newRow("nan") << QStringLiteral("1,nan");
        QTest::newRow("positive-infinity") << QStringLiteral("1,inf");
        QTest::newRow("negative-infinity") << QStringLiteral("1,-inf");
        QTest::newRow("not-a-number") << QStringLiteral("1,strong");
        QTest::newRow("missing") << QStringLiteral("1");
    }

    void rejectsUnsafePower()
    {
        QFETCH(QString, row);
        CpCsvImport::Builder builder;
        QString error;

        QVERIFY(!builder.addRow(row, 2, error));
        QVERIFY2(!error.isEmpty(), "Rejected power had no diagnostic");
        QVERIFY(builder.segments().isEmpty());
    }

    void enforcesWholeFileRowBudget()
    {
        CpCsvImport::Limits limits;
        limits.maxRows = 2;
        limits.maxPoints = 10;
        CpCsvImport::Builder builder(limits);
        QString error;

        QVERIFY(builder.addRow(QStringLiteral("1,100"), 2, error));
        QVERIFY(builder.addRow(QStringLiteral("2,90"), 3, error));
        QVERIFY(!builder.addRow(QStringLiteral("3,80"), 4, error));
        QVERIFY2(error.contains(QStringLiteral("row"), Qt::CaseInsensitive),
                 qPrintable(error));
    }

    void enforcesExpandedPointBudgetBeforeFinalization()
    {
        CpCsvImport::Limits limits;
        limits.maxRows = 10;
        limits.maxPoints = 5;
        CpCsvImport::Builder builder(limits);
        QString error;

        QVERIFY(builder.addRow(QStringLiteral("1,100"), 2, error));
        QVERIFY(!builder.addRow(QStringLiteral("6,90"), 3, error));
        QVERIFY2(error.contains(QStringLiteral("point"), Qt::CaseInsensitive),
                 qPrintable(error));
        QVERIFY(builder.segments().isEmpty());
    }

    void lateFailurePublishesNoSegments()
    {
        CpCsvImport::Builder builder;
        QString error;

        QVERIFY(builder.addRow(QStringLiteral("1,100"), 2, error));
        QVERIFY(builder.addRow(QStringLiteral("5,90"), 3, error));
        QVERIFY(builder.addRow(QStringLiteral("5,80"), 4, error));
        QVERIFY(!builder.finalize(error));
        QVERIFY2(error.contains(QStringLiteral("conflicting"),
                                Qt::CaseInsensitive), qPrintable(error));
        QVERIFY(builder.segments().isEmpty());
        QCOMPARE(builder.pointCount(), 0);
    }

    void rejectsEmptyInput()
    {
        CpCsvImport::Builder builder;
        QString error;

        QVERIFY(!builder.finalize(error));
        QVERIFY2(!error.isEmpty(), "Empty input had no diagnostic");
        QVERIFY(builder.segments().isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestCpCsvImport)
#include "testCpCsvImport.moc"
