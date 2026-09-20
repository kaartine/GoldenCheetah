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

extern int rideFileTestFilterHrvCalls;
extern double rideFileTestFilterHrvMinimum;
extern double rideFileTestFilterHrvMaximum;
extern double rideFileTestFilterHrvRelative;
extern int rideFileTestFilterHrvWindow;

namespace {

#define GC_STRINGIFY_IMPL(value) #value
#define GC_STRINGIFY(value) GC_STRINGIFY_IMPL(value)

void populateDerivedSeriesRide(RideFile &ride)
{
    ride.setTag(QStringLiteral("Sport"), QStringLiteral("Bike"));
    ride.setRecIntSecs(1.0);
    ride.setDataPresent(RideFile::watts, true);
    ride.setDataPresent(RideFile::cad, true);
    ride.setDataPresent(RideFile::kph, true);
    ride.setDataPresent(RideFile::km, true);
    ride.setDataPresent(RideFile::alt, true);

    RideFilePoint first;
    first.secs = 0.0;
    first.watts = 220.0;
    first.cad = 90.0;
    first.kph = 36.0;
    first.km = 0.0;
    first.alt = 100.0;
    ride.appendPoint(first);

    RideFilePoint second = first;
    second.secs = 1.0;
    second.watts = 300.0;
    second.cad = 95.0;
    second.kph = 38.0;
    second.km = 0.01;
    second.alt = 101.0;
    ride.appendPoint(second);
}

void compareDerivedSeries(const RideFile &actual, const RideFile &expected)
{
    QCOMPARE(actual.dataPoints().size(), expected.dataPoints().size());
    for (int index = 0; index < actual.dataPoints().size(); ++index) {
        const RideFilePoint *left = actual.dataPoints().at(index);
        const RideFilePoint *right = expected.dataPoints().at(index);
        QCOMPARE(left->hrd, right->hrd);
        QCOMPARE(left->cadd, right->cadd);
        QCOMPARE(left->kphd, right->kphd);
        QCOMPARE(left->nmd, right->nmd);
        QCOMPARE(left->wattsd, right->wattsd);
        QCOMPARE(left->xp, right->xp);
        QCOMPARE(left->np, right->np);
        QCOMPARE(left->apower, right->apower);
        QCOMPARE(left->atiss, right->atiss);
        QCOMPARE(left->antiss, right->antiss);
        QCOMPARE(left->gear, right->gear);
        QCOMPARE(left->hhb, right->hhb);
        QCOMPARE(left->o2hb, right->o2hb);
        QCOMPARE(left->tcore, right->tcore);
        QCOMPARE(left->clength, right->clength);
        QCOMPARE(left->slope, right->slope);
    }
    const RideFileDataPresent *left = actual.areDataPresent();
    const RideFileDataPresent *right = expected.areDataPresent();
    QCOMPARE(left->np, right->np);
    QCOMPARE(left->xp, right->xp);
    QCOMPARE(left->apower, right->apower);
    QCOMPARE(left->atiss, right->atiss);
    QCOMPARE(left->antiss, right->antiss);
    QCOMPARE(left->gear, right->gear);
    QCOMPARE(left->hhb, right->hhb);
    QCOMPARE(left->o2hb, right->o2hb);
    QCOMPARE(left->tcore, right->tcore);
    QCOMPARE(left->slope, right->slope);
}

} // namespace

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
    void explicitDerivedInputsPreserveLegacyDefaults();
    void explicitDerivedInputsUseConfiguredCp();
    void derivedMetadataOverridesExplicitInputs();
    void absentPowerZonesIgnoreCpMetadata();
    void availablePowerZonesWithoutRangeUseCpMetadata();
    void zeroWheelInputUsesLegacyDefault();
    void cleanDerivedSeriesSkipsExplicitRecalculation();
    void explicitDerivedCalculationHasNoLiveFallback();
    void postProcessPreservesValueInputsAndOrdering();
    void postProcessKeepsParserTimeForNonmatchingFilename();
    void postProcessKeepsLegacyInvalidMatchingTimestamp();
    void postProcessPreservesParsedNotes();
    void postProcessPassesExplicitHrvInputs();
    void postProcessUsesExplicitDerivedInputs();
    void postProcessImplementationHasNoLiveFallback();
    void openRideFileCapturesEffectiveDateBeforePostProcess();
};

void TestRideFileOwnership::postProcessPreservesValueInputsAndOrdering()
{
    RideFile ride(QDateTime(QDate(2001, 2, 3), QTime(4, 5)), 1.0);
    ride.setFileFormat(QStringLiteral("test format"));
    ride.addInterval(
        RideFileInterval::DEVICE, 10.0, 15.0, QStringLiteral("lap"));
    ride.setTag(
        QStringLiteral("lap##Field##Name"), QStringLiteral("ordered"));

    RideFilePoint first;
    first.secs = 10.0;
    first.km = 2.0;
    ride.appendPoint(first);
    RideFilePoint second = first;
    second.secs = 15.0;
    second.km = 3.0;
    ride.appendPoint(second);

    auto *developer = new XDataSeries;
    developer->name = QStringLiteral("DEVELOPER");
    auto *developerPoint = new XDataPoint;
    developerPoint->secs = 12.0;
    developerPoint->km = 2.5;
    developer->datapoints.append(developerPoint);
    ride.addXData(QStringLiteral("DEVELOPER"), developer);

    RideFilePostProcessInputs inputs;
    inputs.orderedIntervalMetadataNames = {
        QStringLiteral("Name"), QStringLiteral("Field##Name")};
    inputs.notes.readable = true;
    inputs.notes.text = QString();
    inputs.athleteTagAvailable = true;
    inputs.athleteName = QString();

    RideFileFactory::instance().postProcessRideFile(
        ride,
        QFileInfo(QStringLiteral("/tmp/2026_09_20_12_34_56.fit")),
        inputs);

    QCOMPARE(ride.startTime(),
             QDateTime(QDate(2026, 9, 20), QTime(12, 34, 56)));
    QCOMPARE(ride.getTag(QStringLiteral("Filename"), QString()),
             QStringLiteral("2026_09_20_12_34_56.fit"));
    QCOMPARE(ride.getTag(QStringLiteral("File Format"), QString()),
             QStringLiteral("test format"));
    QVERIFY(ride.tags().contains(QStringLiteral("Notes")));
    QVERIFY(ride.tags().contains(QStringLiteral("Athlete")));
    QCOMPARE(ride.getTag(QStringLiteral("Year"), QString()),
             QStringLiteral("2026"));
    QCOMPARE(ride.intervals().constFirst()->getTag(
                 QStringLiteral("Name"), QString()),
             QStringLiteral("ordered"));
    QVERIFY(!ride.intervals().constFirst()->tags().contains(
        QStringLiteral("Field##Name")));
    QVERIFY(!ride.tags().contains(QStringLiteral("lap##Field##Name")));
    QCOMPARE(ride.dataPoints().constFirst()->secs, 0.0);
    QCOMPARE(ride.dataPoints().constFirst()->km, 0.0);
    QCOMPARE(ride.dataPoints().constLast()->secs, 5.0);
    QCOMPARE(ride.dataPoints().constLast()->km, 1.0);
    QCOMPARE(ride.intervals().constFirst()->start, 0.0);
    QCOMPARE(ride.intervals().constFirst()->stop, 5.0);
    QCOMPARE(developerPoint->secs, 2.0);
    QCOMPARE(developerPoint->km, 0.5);
}

void TestRideFileOwnership::postProcessPreservesParsedNotes()
{
    RideFile ride;
    ride.setTag(QStringLiteral("Notes"), QStringLiteral("parsed"));
    RideFilePostProcessInputs inputs;
    inputs.notes.readable = true;
    inputs.notes.text = QStringLiteral("sidecar");

    RideFileFactory::instance().postProcessRideFile(
        ride, QFileInfo(QStringLiteral("activity.fit")), inputs);

    QCOMPARE(ride.getTag(QStringLiteral("Notes"), QString()),
             QStringLiteral("parsed"));
}

void TestRideFileOwnership::postProcessKeepsParserTimeForNonmatchingFilename()
{
    const QDateTime parserTime(QDate(2001, 2, 3), QTime(4, 5, 6));
    RideFile ride(parserTime, 1.0);

    RideFileFactory::instance().postProcessRideFile(
        ride,
        QFileInfo(QStringLiteral("activity.fit")),
        RideFilePostProcessInputs {});

    QCOMPARE(ride.startTime(), parserTime);
}

void TestRideFileOwnership::postProcessKeepsLegacyInvalidMatchingTimestamp()
{
    RideFile ride(QDateTime(QDate(2001, 2, 3), QTime(4, 5, 6)), 1.0);

    RideFileFactory::instance().postProcessRideFile(
        ride,
        QFileInfo(QStringLiteral("2026_99_99_99_99_99.fit")),
        RideFilePostProcessInputs {});

    QVERIFY(!ride.startTime().isValid());
}

void TestRideFileOwnership::postProcessPassesExplicitHrvInputs()
{
    RideFile ride;
    auto *hrv = new XDataSeries;
    hrv->name = QStringLiteral("HRV");
    hrv->datapoints.append(new XDataPoint);
    ride.addXData(QStringLiteral("HRV"), hrv);
    RideFilePostProcessInputs inputs;
    inputs.hrv.minimum = 111.0;
    inputs.hrv.maximum = 222.0;
    inputs.hrv.relativeFilter = 0.35;
    inputs.hrv.window = 17;
    rideFileTestFilterHrvCalls = 0;

    RideFileFactory::instance().postProcessRideFile(
        ride, QFileInfo(QStringLiteral("activity.fit")), inputs);

    QCOMPARE(rideFileTestFilterHrvCalls, 1);
    QCOMPARE(rideFileTestFilterHrvMinimum, 111.0);
    QCOMPARE(rideFileTestFilterHrvMaximum, 222.0);
    QCOMPARE(rideFileTestFilterHrvRelative, 0.35);
    QCOMPARE(rideFileTestFilterHrvWindow, 17);
}

void TestRideFileOwnership::postProcessUsesExplicitDerivedInputs()
{
    RideFile actual;
    RideFile expected;
    populateDerivedSeriesRide(actual);
    populateDerivedSeriesRide(expected);
    RideFilePostProcessInputs postInputs;
    postInputs.recalculateDerivedSeries = true;
    postInputs.derivedSeries.powerZonesAvailable = true;
    postInputs.derivedSeries.configuredCp = 275;
    postInputs.derivedSeries.configuredWheelSizeMillimeters = 2300;

    RideFileFactory::instance().postProcessRideFile(
        actual, QFileInfo(QStringLiteral("activity.fit")), postInputs);
    expected.recalculateDerivedSeries(true, postInputs.derivedSeries);

    compareDerivedSeries(actual, expected);
}

void TestRideFileOwnership::postProcessImplementationHasNoLiveFallback()
{
    const QDir root(QString::fromUtf8(GC_STRINGIFY(GC_TEST_SOURCE_ROOT)));
    QFile source(root.filePath(QStringLiteral("src/FileIO/RideFile.cpp")));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray contents = source.readAll();
    const qsizetype implementation = contents.indexOf(
        "RideFileFactory::postProcessRideFile(");
    const qsizetype nextMethod = contents.indexOf(
        "\nvoid\nRideFile::addXData", implementation + 1);
    QVERIFY(implementation >= 0);
    QVERIFY(nextMethod > implementation);
    const QByteArray body = contents.mid(
        implementation, nextMethod - implementation);
    QVERIFY(!body.contains("GlobalContext"));
    QVERIFY(!body.contains("context"));
    QVERIFY(!body.contains("appsettings"));
    QVERIFY(!body.contains("athlete->"));
    QVERIFY(!body.contains("QTextStream"));
    QVERIFY(!body.contains("notesFile"));
}

void TestRideFileOwnership::openRideFileCapturesEffectiveDateBeforePostProcess()
{
    const QDir root(QString::fromUtf8(GC_STRINGIFY(GC_TEST_SOURCE_ROOT)));
    QFile source(root.filePath(QStringLiteral("src/FileIO/RideFile.cpp")));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray contents = source.readAll();
    const qsizetype implementation = contents.indexOf(
        "RideFile *RideFileFactory::openRideFile(");
    const qsizetype nextMethod = contents.indexOf(
        "\nstatic bool\nrideFileStartTimeOverride(", implementation + 1);
    QVERIFY(implementation >= 0);
    QVERIFY(nextMethod > implementation);
    const QByteArray body = contents.mid(
        implementation, nextMethod - implementation);

    const qsizetype attachContext = body.indexOf(
        "result->context = context;");
    const qsizetype resolveEffectiveDate = body.indexOf(
        "const QDate effectiveDate = rideFileStartTimeOverride(");
    const qsizetype captureDerivedInputs = body.indexOf(
        "rideFileDerivedSeriesInputs(*result, effectiveDate);");
    const qsizetype postProcess = body.indexOf(
        "postProcessRideFile(*result, fileInfo, postInputs);");
    QVERIFY(attachContext >= 0);
    QVERIFY(resolveEffectiveDate > attachContext);
    QVERIFY(captureDerivedInputs > resolveEffectiveDate);
    QVERIFY(postProcess > captureDerivedInputs);
    const QByteArray effectiveDateSelection = body.mid(
        resolveEffectiveDate,
        captureDerivedInputs - resolveEffectiveDate);
    QVERIFY(effectiveDateSelection.contains("? filenameStartTime.date()"));
    QVERIFY(effectiveDateSelection.contains(
        ": result->startTime().date();"));
}

void TestRideFileOwnership::explicitDerivedInputsPreserveLegacyDefaults()
{
    RideFile legacy;
    RideFile explicitInputs;
    populateDerivedSeriesRide(legacy);
    populateDerivedSeriesRide(explicitInputs);

    legacy.recalculateDerivedSeries(true);
    explicitInputs.recalculateDerivedSeries(
        true, RideFileDerivedSeriesInputs {});

    compareDerivedSeries(explicitInputs, legacy);
}

void TestRideFileOwnership::explicitDerivedInputsUseConfiguredCp()
{
    RideFile lowerCp;
    RideFile higherCp;
    populateDerivedSeriesRide(lowerCp);
    populateDerivedSeriesRide(higherCp);

    RideFileDerivedSeriesInputs lowerInputs;
    lowerInputs.powerZonesAvailable = true;
    lowerInputs.configuredCp = 200;
    RideFileDerivedSeriesInputs higherInputs = lowerInputs;
    higherInputs.configuredCp = 300;
    lowerCp.recalculateDerivedSeries(true, lowerInputs);
    higherCp.recalculateDerivedSeries(true, higherInputs);

    QVERIFY(lowerCp.dataPoints().constLast()->atiss > 0.0);
    QVERIFY(higherCp.dataPoints().constLast()->atiss > 0.0);
    QVERIFY(lowerCp.dataPoints().constLast()->atiss
            != higherCp.dataPoints().constLast()->atiss);
    QVERIFY(lowerCp.dataPoints().constLast()->antiss
            != higherCp.dataPoints().constLast()->antiss);
}

void TestRideFileOwnership::derivedMetadataOverridesExplicitInputs()
{
    RideFile metadataOverride;
    RideFile explicitEquivalent;
    populateDerivedSeriesRide(metadataOverride);
    populateDerivedSeriesRide(explicitEquivalent);
    metadataOverride.setTag(QStringLiteral("CP"), QStringLiteral("300"));
    metadataOverride.setTag(
        QStringLiteral("Wheelsize"), QStringLiteral("2500"));

    RideFileDerivedSeriesInputs lowerInputs;
    lowerInputs.powerZonesAvailable = true;
    lowerInputs.configuredCp = 200;
    lowerInputs.configuredWheelSizeMillimeters = 2000;
    RideFileDerivedSeriesInputs equivalentInputs;
    equivalentInputs.configuredCp = 300;
    equivalentInputs.configuredWheelSizeMillimeters = 2500;
    metadataOverride.recalculateDerivedSeries(true, lowerInputs);
    explicitEquivalent.recalculateDerivedSeries(true, equivalentInputs);

    compareDerivedSeries(metadataOverride, explicitEquivalent);
}

void TestRideFileOwnership::absentPowerZonesIgnoreCpMetadata()
{
    RideFile taggedWithoutZones;
    RideFile noCp;
    populateDerivedSeriesRide(taggedWithoutZones);
    populateDerivedSeriesRide(noCp);
    taggedWithoutZones.setTag(QStringLiteral("CP"), QStringLiteral("300"));

    taggedWithoutZones.recalculateDerivedSeries(
        true, RideFileDerivedSeriesInputs {});
    noCp.recalculateDerivedSeries(
        true, RideFileDerivedSeriesInputs {});

    compareDerivedSeries(taggedWithoutZones, noCp);
}

void TestRideFileOwnership::availablePowerZonesWithoutRangeUseCpMetadata()
{
    RideFile taggedWithoutRange;
    RideFile explicitEquivalent;
    populateDerivedSeriesRide(taggedWithoutRange);
    populateDerivedSeriesRide(explicitEquivalent);
    taggedWithoutRange.setTag(QStringLiteral("CP"), QStringLiteral("300"));

    RideFileDerivedSeriesInputs noRange;
    noRange.powerZonesAvailable = true;
    RideFileDerivedSeriesInputs equivalent = noRange;
    equivalent.configuredCp = 300;
    taggedWithoutRange.recalculateDerivedSeries(true, noRange);
    explicitEquivalent.recalculateDerivedSeries(true, equivalent);

    compareDerivedSeries(taggedWithoutRange, explicitEquivalent);
}

void TestRideFileOwnership::zeroWheelInputUsesLegacyDefault()
{
    RideFile zeroInput;
    RideFile legacyDefault;
    populateDerivedSeriesRide(zeroInput);
    populateDerivedSeriesRide(legacyDefault);

    RideFileDerivedSeriesInputs zero;
    zero.configuredWheelSizeMillimeters = 0;
    zeroInput.recalculateDerivedSeries(true, zero);
    legacyDefault.recalculateDerivedSeries(
        true, RideFileDerivedSeriesInputs {});

    compareDerivedSeries(zeroInput, legacyDefault);
}

void TestRideFileOwnership::cleanDerivedSeriesSkipsExplicitRecalculation()
{
    RideFile ride;
    populateDerivedSeriesRide(ride);
    RideFileDerivedSeriesInputs initial;
    initial.configuredWheelSizeMillimeters = 2100;
    ride.recalculateDerivedSeries(true, initial);
    const double initialGear = ride.dataPoints().constLast()->gear;

    RideFileDerivedSeriesInputs replacement = initial;
    replacement.configuredWheelSizeMillimeters = 1000;
    ride.recalculateDerivedSeries(false, replacement);

    QCOMPARE(ride.dataPoints().constLast()->gear, initialGear);
}

void TestRideFileOwnership::explicitDerivedCalculationHasNoLiveFallback()
{
    const QDir root(QString::fromUtf8(GC_STRINGIFY(GC_TEST_SOURCE_ROOT)));
    QFile source(root.filePath(QStringLiteral("src/FileIO/RideFile.cpp")));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray contents = source.readAll();
    const qsizetype implementation = contents.indexOf(
        "RideFile::recalculateDerivedSeriesImpl(");
    const qsizetype nextMethod = contents.indexOf(
        "\nRideFile::", implementation + 1);
    QVERIFY(implementation >= 0);
    QVERIFY(nextMethod > implementation);
    const QByteArray body = contents.mid(
        implementation, nextMethod - implementation);
    QVERIFY(!body.contains("context"));
    QVERIFY(!body.contains("Athlete"));
    QVERIFY(!body.contains("athlete"));
    QVERIFY(!body.contains("appsettings"));
}

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
