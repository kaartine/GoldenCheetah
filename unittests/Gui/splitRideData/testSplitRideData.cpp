#include <QtTest>

#include "RideFile.h"
#include "SplitRideData.h"

#include <memory>

namespace {

using OwnedRide = std::unique_ptr<RideFile>;

class SourceRide
{
public:
    SourceRide()
    {
        ride.setStartTime(
            QDateTime(QDate(2026, 7, 6), QTime(8, 0)));
        ride.setRecIntSecs(10.0);
        ride.setDeviceType(QStringLiteral("test device"));
        ride.setFileFormat(QStringLiteral("json"));
        ride.setTag(QStringLiteral("Workout Code"), QStringLiteral("split"));
        ride.setTag(
            QStringLiteral("Linked Filename"),
            QStringLiteral("other.json"));

        for (int index = 0; index < 4; ++index) {
            RideFilePoint point;
            point.secs = index * 10.0;
            point.km = index * 0.25;
            point.hr = 120.0 + index;
            ride.appendPoint(point);
        }

        XDataSeries *series = new XDataSeries;
        series->name = QStringLiteral("TEST");
        series->valuename << QStringLiteral("value");
        series->unitname << QStringLiteral("unit");
        series->valuetype << RideFile::none;
        for (int index = 0; index < 4; ++index) {
            XDataPoint *point = new XDataPoint;
            point->secs = index * 10.0;
            point->km = index * 0.25;
            point->number[0] = 1000.0 + index;
            series->datapoints.append(point);
        }
        ride.addXData(series->name, series);
    }

    RideFile ride;
};

QList<double> pointHeartRates(const RideFile &ride)
{
    QList<double> values;
    for (const RideFilePoint *point : ride.dataPoints()) {
        values.append(point->hr);
    }
    return values;
}

QList<double> xdataValues(RideFile &ride)
{
    QList<double> values;
    const XDataSeries *series =
        ride.xdata().value(QStringLiteral("TEST"));
    if (!series) {
        return values;
    }
    for (const XDataPoint *point : series->datapoints) {
        values.append(point->number[0]);
    }
    return values;
}

} // namespace

class TestSplitRideData : public QObject
{
    Q_OBJECT

private slots:
    void adjacentSegmentsOwnBoundaryExactlyOnce();
    void includedEndRetainsFinalSelectedSample();
    void xdataMetadataAndValuesAreCopiedOnce();
    void adjacentSegmentsOwnIntervalBoundaryExactlyOnce();
    void truncatedIntervalUsesSegmentLocalBounds();
    void invalidRangesAreRejected_data();
    void invalidRangesAreRejected();
};

void TestSplitRideData::adjacentSegmentsOwnBoundaryExactlyOnce()
{
    SourceRide source;

    OwnedRide first(extractSplitRideSegment(
        source.ride, 0, 2, SplitSegmentEnd::Exclude));
    OwnedRide second(extractSplitRideSegment(
        source.ride, 2, 3, SplitSegmentEnd::Include));

    QVERIFY(first);
    QVERIFY(second);
    QCOMPARE(pointHeartRates(*first), QList<double>({120.0, 121.0}));
    QCOMPARE(pointHeartRates(*second), QList<double>({122.0, 123.0}));
    QCOMPARE(xdataValues(*first), QList<double>({1000.0, 1001.0}));
    QCOMPARE(xdataValues(*second), QList<double>({1002.0, 1003.0}));
}

void TestSplitRideData::includedEndRetainsFinalSelectedSample()
{
    SourceRide source;

    OwnedRide segment(extractSplitRideSegment(
        source.ride, 1, 3, SplitSegmentEnd::Include));

    QVERIFY(segment);
    QCOMPARE(pointHeartRates(*segment),
             QList<double>({121.0, 122.0, 123.0}));
    QCOMPARE(segment->dataPoints().constLast()->secs, 20.0);
    QCOMPARE(segment->dataPoints().constLast()->km, 0.5);
}

void TestSplitRideData::xdataMetadataAndValuesAreCopiedOnce()
{
    SourceRide source;

    OwnedRide segment(extractSplitRideSegment(
        source.ride, 1, 3, SplitSegmentEnd::Include));

    QVERIFY(segment);
    QCOMPARE(segment->xdata().size(), 1);
    const XDataSeries *series =
        segment->xdata().value(QStringLiteral("TEST"));
    QVERIFY(series);
    QCOMPARE(series->name, QStringLiteral("TEST"));
    QCOMPARE(
        series->valuename,
        QStringList({QStringLiteral("value")}));
    QCOMPARE(
        series->unitname,
        QStringList({QStringLiteral("unit")}));
    QCOMPARE(series->valuetype.size(), 1);
    QCOMPARE(series->datapoints.size(), 3);
    QCOMPARE(series->datapoints.constFirst()->secs, 0.0);
    QCOMPARE(series->datapoints.constLast()->secs, 20.0);
    QCOMPARE(xdataValues(*segment),
             QList<double>({1001.0, 1002.0, 1003.0}));
}

void TestSplitRideData::adjacentSegmentsOwnIntervalBoundaryExactlyOnce()
{
    SourceRide source;
    source.ride.addInterval(
        RideFileInterval::USER,
        20.0,
        25.0,
        QStringLiteral("boundary"));

    OwnedRide first(extractSplitRideSegment(
        source.ride, 0, 2, SplitSegmentEnd::Exclude));
    OwnedRide second(extractSplitRideSegment(
        source.ride, 2, 3, SplitSegmentEnd::Include));

    QVERIFY(first);
    QVERIFY(second);
    QVERIFY(first->intervals().isEmpty());
    QCOMPARE(second->intervals().size(), 1);
    QCOMPARE(second->intervals().constFirst()->start, 0.0);
    QCOMPARE(second->intervals().constFirst()->stop, 5.0);
}

void TestSplitRideData::truncatedIntervalUsesSegmentLocalBounds()
{
    SourceRide source;
    source.ride.addInterval(
        RideFileInterval::USER,
        15.0,
        35.0,
        QStringLiteral("crossing"));

    OwnedRide segment(extractSplitRideSegment(
        source.ride, 1, 3, SplitSegmentEnd::Include));

    QVERIFY(segment);
    QCOMPARE(segment->intervals().size(), 1);
    const RideFileInterval *interval =
        segment->intervals().constFirst();
    QCOMPARE(interval->start, 5.0);
    QCOMPARE(interval->stop, 20.0);
    QCOMPARE(interval->name, QStringLiteral("crossing"));
}

void TestSplitRideData::invalidRangesAreRejected_data()
{
    QTest::addColumn<long>("start");
    QTest::addColumn<long>("stop");

    QTest::newRow("negative-start") << -1L << 2L;
    QTest::newRow("negative-stop") << 0L << -1L;
    QTest::newRow("reversed") << 2L << 1L;
    QTest::newRow("empty") << 2L << 2L;
    QTest::newRow("start-outside") << 4L << 4L;
    QTest::newRow("stop-outside") << 0L << 4L;
}

void TestSplitRideData::invalidRangesAreRejected()
{
    QFETCH(long, start);
    QFETCH(long, stop);
    SourceRide source;

    OwnedRide segment(extractSplitRideSegment(
        source.ride, start, stop, SplitSegmentEnd::Include));

    QVERIFY(!segment);
}

QTEST_MAIN(TestSplitRideData)
#include "testSplitRideData.moc"
