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

class TestableRideFile : public RideFile
{
public:
    using RideFile::clearIntervals;
    using RideFile::fillInIntervals;
};

class TestRideFileOwnership : public QObject
{
    Q_OBJECT

private slots:
    void allConstructorsReleaseSummaryPoints();
    void copyOwnsIndependentReferencePoints();
    void removalsReleaseReferencePoints();
    void destructorReleasesIntervals();
    void copyOwnsIndependentIntervals();
    void clearRebuildAndRemovalReleaseIntervals();
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

void TestRideFileOwnership::destructorReleasesIntervals()
{
    // Repetition makes a missing destructor release deterministic under LSan.
    for (int iteration = 0; iteration < 32; ++iteration) {
        RideFile ride;
        ride.addInterval(RideFileInterval::DEVICE, 10.0, 20.0,
                         QStringLiteral("lap"));
        QCOMPARE(ride.intervals().size(), 1);
    }
}

void TestRideFileOwnership::copyOwnsIndependentIntervals()
{
    RideFile *source = new RideFile;
    source->addInterval(RideFileInterval::USER, 10.0, 20.0,
                        QStringLiteral("effort"), Qt::red, true);
    source->intervals().constFirst()->setTag(
        QStringLiteral("source"), QStringLiteral("manual"));

    const RideFileInterval *sourceInterval =
        source->intervals().constFirst();
    RideFile copy(source);

    QCOMPARE(copy.intervals().size(), 1);
    const RideFileInterval *copyInterval = copy.intervals().constFirst();
    QVERIFY(copyInterval != sourceInterval);
    QCOMPARE(copyInterval->type, RideFileInterval::USER);
    QCOMPARE(copyInterval->start, 10.0);
    QCOMPARE(copyInterval->stop, 20.0);
    QCOMPARE(copyInterval->name, QStringLiteral("effort"));
    QCOMPARE(copyInterval->color, QColor(Qt::red));
    QVERIFY(copyInterval->test);
    QCOMPARE(copyInterval->getTag(QStringLiteral("source"), QString()),
             QStringLiteral("manual"));

    delete source;
    QCOMPARE(copyInterval->name, QStringLiteral("effort"));
}

void TestRideFileOwnership::clearRebuildAndRemovalReleaseIntervals()
{
    for (int iteration = 0; iteration < 32; ++iteration) {
        TestableRideFile ride;
        ride.addInterval(RideFileInterval::DEVICE, 1.0, 2.0,
                         QStringLiteral("discarded"));
        ride.clearIntervals();
        QVERIFY(ride.intervals().isEmpty());

        RideFilePoint first;
        first.secs = 0.0;
        first.interval = 1;
        ride.appendPoint(first);

        RideFilePoint second;
        second.secs = 1.0;
        second.interval = 2;
        ride.appendPoint(second);

        ride.addInterval(RideFileInterval::DEVICE, 2.0, 3.0,
                         QStringLiteral("replaced"));
        ride.fillInIntervals();
        QVERIFY(!ride.intervals().isEmpty());

        RideFileInterval *interval = ride.intervals().constFirst();
        QVERIFY(ride.removeInterval(interval));
    }
}

QTEST_GUILESS_MAIN(TestRideFileOwnership)

#include "testRideFileOwnership.moc"
