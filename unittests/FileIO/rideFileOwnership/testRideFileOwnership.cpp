/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

#include "RideFile.h"

class TestRideFileOwnership : public QObject
{
    Q_OBJECT

private slots:
    void allConstructorsReleaseSummaryPoints();
};

void TestRideFileOwnership::allConstructorsReleaseSummaryPoints()
{
    // Repetition makes a missing destructor release deterministic under LSan.
    for (int iteration = 0; iteration < 32; ++iteration) {
        RideFile defaultRide;
        defaultRide.setTag(QStringLiteral("Constructor"),
                           QStringLiteral("default"));
        QCOMPARE(defaultRide.getTag(QStringLiteral("Constructor"), QString()),
                 QStringLiteral("default"));

        const QDateTime start(QDate(2026, 7, 6), QTime(10, 0));
        RideFile datedRide(
            start, 1.0);
        datedRide.setTag(QStringLiteral("Constructor"),
                         QStringLiteral("dated"));
        QCOMPARE(datedRide.startTime(), start);

        RideFile copiedRide(&datedRide);
        QCOMPARE(copiedRide.startTime(), start);
        QCOMPARE(copiedRide.getTag(QStringLiteral("Constructor"), QString()),
                 QStringLiteral("dated"));
    }
}

QTEST_GUILESS_MAIN(TestRideFileOwnership)

#include "testRideFileOwnership.moc"
