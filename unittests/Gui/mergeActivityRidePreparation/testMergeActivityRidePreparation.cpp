#include <QtTest>

#include "MergeActivityRidePreparation.h"

#include "RideFile.h"

class TestMergeActivityRidePreparation : public QObject
{
    Q_OBJECT

private slots:
    void failedResampleKeepsWorkingRide();
    void successfulResampleReplacesWorkingRide();
    void nullSourceClearsWorkingRide();
};

void TestMergeActivityRidePreparation::failedResampleKeepsWorkingRide()
{
    RideFile source;
    source.setRecIntSecs(2.0);
    source.setDataPresent(RideFile::hr, true);
    RideFilePoint sample;
    sample.secs = 0.0;
    sample.hr = 150.0;
    source.appendPoint(sample);

    RideFile *working = new RideFile;
    RideFile *previous = working;
    working->setTag(QStringLiteral("Marker"), QStringLiteral("previous"));
    QSignalSpy previousDeleted(previous, &RideFile::deleted);

    QVERIFY(!MergeActivityRidePreparation::replaceWorkingRide(
        working, &source, 1.0));
    QCOMPARE(working, previous);
    QCOMPARE(working->getTag(QStringLiteral("Marker"), QString()),
             QStringLiteral("previous"));
    QCOMPARE(previousDeleted.count(), 0);

    delete working;

    working = nullptr;
    QVERIFY(!MergeActivityRidePreparation::replaceWorkingRide(
        working, &source, 1.0));
    QCOMPARE(working, nullptr);
}

void TestMergeActivityRidePreparation::successfulResampleReplacesWorkingRide()
{
    RideFile source;
    source.setRecIntSecs(1.0);
    source.setDataPresent(RideFile::hr, true);
    RideFilePoint sample;
    sample.secs = 5.0;
    sample.hr = 151.0;
    source.appendPoint(sample);

    XDataSeries *sourceXData = new XDataSeries;
    sourceXData->name = QStringLiteral("EXTRA");
    XDataPoint *sourceXDataPoint = new XDataPoint;
    sourceXDataPoint->secs = 5.0;
    sourceXDataPoint->number[0] = 42.0;
    sourceXData->datapoints.append(sourceXDataPoint);
    source.addXData(sourceXData->name, sourceXData);

    RideFile *working = new RideFile;
    RideFile *previous = working;
    QSignalSpy previousDeleted(previous, &RideFile::deleted);

    QVERIFY(MergeActivityRidePreparation::replaceWorkingRide(
        working, &source, 1.0));
    QVERIFY(working != previous);
    QCOMPARE(previousDeleted.count(), 1);
    QCOMPARE(working->dataPoints().size(), 1);
    QCOMPARE(working->dataPoints().constFirst()->hr, 151.0);

    const XDataSeries *copiedXData = working->xdata(QStringLiteral("EXTRA"));
    QVERIFY(copiedXData);
    QVERIFY(copiedXData != sourceXData);
    QCOMPARE(copiedXData->datapoints.size(), 1);
    QVERIFY(copiedXData->datapoints.constFirst() != sourceXDataPoint);
    QCOMPARE(copiedXData->datapoints.constFirst()->number[0], 42.0);

    delete working;
}

void TestMergeActivityRidePreparation::nullSourceClearsWorkingRide()
{
    RideFile *working = new RideFile;
    RideFile *previous = working;
    QSignalSpy previousDeleted(previous, &RideFile::deleted);

    QVERIFY(!MergeActivityRidePreparation::replaceWorkingRide(
        working, nullptr, 1.0));
    QCOMPARE(working, nullptr);
    QCOMPARE(previousDeleted.count(), 1);
}

QTEST_GUILESS_MAIN(TestMergeActivityRidePreparation)
#include "testMergeActivityRidePreparation.moc"
