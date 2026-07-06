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
    void copyOwnsIndependentReferencePoints();
    void removalsReleaseReferencePoints();
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

void TestRideFileOwnership::copyOwnsIndependentReferencePoints()
{
    RideFile *source = new RideFile;
    RideFilePoint reference;
    reference.secs = 42.0;
    reference.hr = 151.0;
    source->appendReference(reference);

    const RideFilePoint *sourceReference =
        source->referencePoints().constFirst();
    RideFile copy(source);

    QCOMPARE(copy.referencePoints().size(), 1);
    const RideFilePoint *copyReference =
        copy.referencePoints().constFirst();
    QVERIFY(copyReference != sourceReference);
    QCOMPARE(copyReference->secs, 42.0);
    QCOMPARE(copyReference->hr, 151.0);

    delete source;
    QCOMPARE(copyReference->secs, 42.0);
    QCOMPARE(copyReference->hr, 151.0);
}

void TestRideFileOwnership::removalsReleaseReferencePoints()
{
    RideFile ride;
    RideFilePoint reference;
    reference.secs = 42.0;
    ride.appendReference(reference);
    reference.secs = 0.0;
    ride.appendReference(reference);

    ride.removeExhaustion(0);
    QCOMPARE(ride.referencePoints().size(), 1);
    QCOMPARE(ride.referencePoints().constFirst()->secs, 0.0);

    ride.removeReference(0);
    QVERIFY(ride.referencePoints().isEmpty());
}

QTEST_GUILESS_MAIN(TestRideFileOwnership)

#include "testRideFileOwnership.moc"
