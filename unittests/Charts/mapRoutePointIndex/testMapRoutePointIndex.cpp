/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "Charts/MapRoutePointIndex.h"

#include <QElapsedTimer>
#include <QTest>

#include <limits>

class TestMapRoutePointIndex : public QObject
{
    Q_OBJECT

private slots:
    void rejectsInvalidInput();
    void scalesLongitudeByLatitude_data();
    void scalesLongitudeByLatitude();
    void preservesRouteOrderForDistanceTies();
    void queryWorkIsSublinear_data();
    void queryWorkIsSublinear();
};

void TestMapRoutePointIndex::rejectsInvalidInput()
{
    MapRoutePointIndex index;
    QVERIFY(!index.append(
        std::numeric_limits<double>::quiet_NaN(), 0.0, 0));
    QVERIFY(!index.append(91.0, 0.0, 0));
    QVERIFY(!index.append(0.0, 181.0, 0));
    QVERIFY(!index.append(0.0, 0.0, -1));
    index.finalize();
    QCOMPARE(index.nearest(0.0, 0.0), qsizetype(-1));
    QCOMPARE(index.nearest(91.0, 0.0), qsizetype(-1));
}

void TestMapRoutePointIndex::scalesLongitudeByLatitude_data()
{
    QTest::addColumn<double>("latitude");
    QTest::addColumn<qsizetype>("expected");

    QTest::newRow("equator") << 0.0 << qsizetype(0);
    QTest::newRow("north-high-latitude") << 60.0 << qsizetype(1);
    QTest::newRow("south-high-latitude") << -60.0 << qsizetype(1);
}

void TestMapRoutePointIndex::scalesLongitudeByLatitude()
{
    QFETCH(double, latitude);
    QFETCH(qsizetype, expected);

    MapRoutePointIndex index;
    QVERIFY(index.append(latitude + 0.0006, 0.0, 0));
    QVERIFY(index.append(latitude, 0.0009, 1));
    index.finalize();

    QCOMPARE(index.nearest(latitude, 0.0), expected);
}

void TestMapRoutePointIndex::preservesRouteOrderForDistanceTies()
{
    MapRoutePointIndex index;
    QVERIFY(index.append(0.0005, 20.0, 0));
    QVERIFY(index.append(-0.0005, 20.0, 1));
    index.finalize();

    QCOMPARE(index.nearest(0.0, 20.0), qsizetype(0));
}

void TestMapRoutePointIndex::queryWorkIsSublinear_data()
{
    QTest::addColumn<int>("pointCount");

    QTest::newRow("10k") << 10000;
    QTest::newRow("100k") << 100000;
    QTest::newRow("1m") << 1000000;
}

void TestMapRoutePointIndex::queryWorkIsSublinear()
{
    QFETCH(int, pointCount);
    MapRoutePointIndex index;
    index.reserve(pointCount);
    bool appended = true;
    for (int sourceIndex = 0;
         sourceIndex < pointCount;
         ++sourceIndex) {
        const double fraction = pointCount == 1
            ? 0.0
            : static_cast<double>(sourceIndex)
                / static_cast<double>(pointCount - 1);
        appended = index.append(
            -80.0 + fraction * 160.0,
            24.0,
            sourceIndex) && appended;
    }
    QVERIFY(appended);

    QElapsedTimer timer;
    timer.start();
    index.finalize();
    const qint64 buildMilliseconds = timer.elapsed();
    const int targetIndex = pointCount * 7 / 8;
    const double targetLatitude = -80.0
        + static_cast<double>(targetIndex)
            / static_cast<double>(pointCount - 1)
            * 160.0;
    qsizetype examined = 0;
    timer.restart();
    const qsizetype nearest =
        index.nearest(targetLatitude, 24.0, &examined);
    const qint64 queryNanoseconds = timer.nsecsElapsed();

    QCOMPARE(nearest, qsizetype(targetIndex));
    QVERIFY2(
        examined < 32,
        qPrintable(QStringLiteral(
            "query examined %1 of %2 points")
            .arg(examined)
            .arg(pointCount)));
    qInfo("indexed %d points in %lld ms; query examined %lld in %lld ns",
          pointCount,
          static_cast<long long>(buildMilliseconds),
          static_cast<long long>(examined),
          static_cast<long long>(queryNanoseconds));
}

QTEST_GUILESS_MAIN(TestMapRoutePointIndex)

#include "testMapRoutePointIndex.moc"
