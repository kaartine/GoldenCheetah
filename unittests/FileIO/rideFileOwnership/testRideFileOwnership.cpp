/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

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
    void destructorReleasesCalibrations();
    void copyOwnsIndependentCalibrations();
    void duplicateTimestampUpdateKeepsOwnedPointAndSummaries();
    void fileCrcReleasesItsReadStream();
    void fileCrcDistinguishesEmptyFileFromReadFailure();
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

void TestRideFileOwnership::destructorReleasesCalibrations()
{
    // Repetition makes a missing destructor release deterministic under LSan.
    for (int iteration = 0; iteration < 32; ++iteration) {
        RideFile ride;
        ride.addCalibration(10.0, 123, QStringLiteral("zero"));
        QCOMPARE(ride.calibrations().size(), 1);
    }
}

void TestRideFileOwnership::copyOwnsIndependentCalibrations()
{
    RideFile *source = new RideFile;
    source->addCalibration(10.0, 123, QStringLiteral("zero"));

    const RideFileCalibration *sourceCalibration =
        source->calibrations().constFirst();
    RideFile copy(source);

    QCOMPARE(copy.calibrations().size(), 1);
    const RideFileCalibration *copyCalibration =
        copy.calibrations().constFirst();
    QVERIFY(copyCalibration != sourceCalibration);
    QCOMPARE(copyCalibration->start, 10.0);
    QCOMPARE(copyCalibration->value, 123);
    QCOMPARE(copyCalibration->name, QStringLiteral("zero"));

    delete source;
    QCOMPARE(copyCalibration->name, QStringLiteral("zero"));
}

void TestRideFileOwnership::duplicateTimestampUpdateKeepsOwnedPointAndSummaries()
{
    RideFile ride;

    ride.appendOrUpdatePoint(
        1.0, 80.0, 0.0, 0.1, 30.0, 0.0, 200.0, 100.0,
        0.0, 0.0, 0.0, 0.0, RideFile::NA, RideFile::NA,
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0, false);

    QCOMPARE(ride.dataPoints().size(), 1);
    const RideFilePoint *const ownedPoint = ride.dataPoints().constFirst();

    ride.appendOrUpdatePoint(
        1.0, 0.0, 151.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0, RideFile::NA, RideFile::NA,
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0, false);

    QCOMPARE(ride.dataPoints().size(), 1);
    QCOMPARE(ride.dataPoints().constFirst(), ownedPoint);
    QCOMPARE(ownedPoint->cad, 80.0);
    QCOMPARE(ownedPoint->hr, 151.0);
    QCOMPARE(ownedPoint->watts, 200.0);
    QCOMPARE(ride.getMinPoint(RideFile::hr).toDouble(), 151.0);
    QCOMPARE(ride.getAvgPoint(RideFile::hr).toDouble(), 151.0);
    QCOMPARE(ride.getMaxPoint(RideFile::hr).toDouble(), 151.0);
    QCOMPARE(ride.getAvgPoint(RideFile::watts).toDouble(), 200.0);
}

void TestRideFileOwnership::fileCrcReleasesItsReadStream()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("activity.fit"));
    const QByteArray contents("GoldenCheetah CRC regression");

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(contents), contents.size());
    file.close();

    const unsigned int expected = qChecksum(QByteArrayView(contents));
    // Repetition makes the stream leak deterministic under LSan.
    for (int iteration = 0; iteration < 64; ++iteration) {
        unsigned int checksum = 0;
        QVERIFY(RideFile::computeFileCRC(
            path, checksum));
        QCOMPARE(checksum, expected);
    }
}

void TestRideFileOwnership::fileCrcDistinguishesEmptyFileFromReadFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString emptyPath =
        directory.filePath(QStringLiteral("empty.fit"));
    QFile emptyFile(emptyPath);
    QVERIFY(emptyFile.open(QIODevice::WriteOnly));
    emptyFile.close();

    unsigned int checksum = 0xbeef;
    QVERIFY(RideFile::computeFileCRC(emptyPath, checksum));
    QCOMPARE(checksum, 0U);

    checksum = 0xbeef;
    QVERIFY(!RideFile::computeFileCRC(
        directory.filePath(QStringLiteral("missing.fit")),
        checksum));
    QCOMPARE(checksum, 0xbeefU);
}

QTEST_GUILESS_MAIN(TestRideFileOwnership)

#include "testRideFileOwnership.moc"
