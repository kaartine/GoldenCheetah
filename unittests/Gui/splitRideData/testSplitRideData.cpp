#include <QtTest>

#include "RideFile.h"
#include "SplitActivityWorkflow.h"
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
    void sourcePreflightRunsBeforeRefresh();
    void rejectedSourcePreflightDoesNotRefresh();
    void keptSourceNeedsNoRemovalPreflight();
    void changedSourceIsRejectedBeforeRemovalPreflight();
    void sourceChangedDuringRemovalPreflightDoesNotRefresh();
    void linkedSourceRemovalPolicy_data();
    void linkedSourceRemovalPolicy();
    void sourceIdentityAcceptsExactMatch();
    void sourceIdentityRequiresLiveWorkflowOwners_data();
    void sourceIdentityRequiresLiveWorkflowOwners();
    void sourceIdentityRejectsMutation_data();
    void sourceIdentityRejectsMutation();
    void sourceContentChangeRejectsStaleSplit();
    void postPublishRevalidationStopsAfterRecoveryDialog();
};

void TestSplitRideData::sourcePreflightRunsBeforeRefresh()
{
    QStringList events;

    QVERIFY(prepareSplitSourceBeforeSelection(
        false,
        [&] {
            events.append(QStringLiteral("identity"));
            return true;
        },
        [&] {
            events.append(QStringLiteral("preflight"));
            return true;
        },
        [&] {
            events.append(QStringLiteral("refresh"));
        }));

    QCOMPARE(
        events,
        QStringList({
            QStringLiteral("identity"),
            QStringLiteral("preflight"),
            QStringLiteral("identity"),
            QStringLiteral("refresh"),
            QStringLiteral("identity")}));
}

void TestSplitRideData::
rejectedSourcePreflightDoesNotRefresh()
{
    int refreshCalls = 0;

    QVERIFY(!prepareSplitSourceBeforeSelection(
        false,
        [] { return true; },
        [] { return false; },
        [&] { ++refreshCalls; }));

    QCOMPARE(refreshCalls, 0);
}

void TestSplitRideData::keptSourceNeedsNoRemovalPreflight()
{
    int preflightCalls = 0;
    int refreshCalls = 0;

    QVERIFY(prepareSplitSourceBeforeSelection(
        true,
        [] { return true; },
        [&] {
            ++preflightCalls;
            return false;
        },
        [&] { ++refreshCalls; }));

    QCOMPARE(preflightCalls, 0);
    QCOMPARE(refreshCalls, 0);
}

void TestSplitRideData::
changedSourceIsRejectedBeforeRemovalPreflight()
{
    int preflightCalls = 0;
    int refreshCalls = 0;

    QVERIFY(!prepareSplitSourceBeforeSelection(
        false,
        [] { return false; },
        [&] {
            ++preflightCalls;
            return true;
        },
        [&] { ++refreshCalls; }));

    QCOMPARE(preflightCalls, 0);
    QCOMPARE(refreshCalls, 0);
}

void TestSplitRideData::
sourceChangedDuringRemovalPreflightDoesNotRefresh()
{
    bool sourceCurrent = true;
    int refreshCalls = 0;

    QVERIFY(!prepareSplitSourceBeforeSelection(
        false,
        [&] { return sourceCurrent; },
        [&] {
            sourceCurrent = false;
            return true;
        },
        [&] { ++refreshCalls; }));

    QCOMPARE(refreshCalls, 0);
}

void TestSplitRideData::linkedSourceRemovalPolicy_data()
{
    QTest::addColumn<bool>("keepOriginal");
    QTest::addColumn<bool>("hasLinkedActivity");
    QTest::addColumn<bool>("allowed");

    QTest::newRow("remove-linked-source")
        << false << true << false;
    QTest::newRow("keep-linked-source")
        << true << true << true;
    QTest::newRow("remove-unlinked-source")
        << false << false << true;
}

void TestSplitRideData::linkedSourceRemovalPolicy()
{
    QFETCH(bool, keepOriginal);
    QFETCH(bool, hasLinkedActivity);
    QFETCH(bool, allowed);

    QCOMPARE(
        splitActivitySourceRemovalAllowed(
            keepOriginal, hasLinkedActivity),
        allowed);
}

void TestSplitRideData::sourceIdentityAcceptsExactMatch()
{
    const SplitActivitySourceIdentity identity{
        QStringLiteral("source.json"),
        QStringLiteral("/activities"),
        false};

    QVERIFY(splitActivitySourceIsCurrent(
        true, true, true, true, identity, identity));
}

void TestSplitRideData::sourceContentChangeRejectsStaleSplit()
{
    const SplitActivitySourceIdentity identity {
        QStringLiteral("source.json"),
        QStringLiteral("/activities"), false};
    const SplitActivityContentSnapshot expected {
        quintptr(0x1234), 7};
    const SplitActivityContentSnapshot changed {
        quintptr(0x1234), 8};

    QVERIFY(!splitActivitySourceSnapshotIsCurrent(
        true, true, true, true,
        identity, identity, expected, changed));
}

void TestSplitRideData::
sourceIdentityRequiresLiveWorkflowOwners_data()
{
    QTest::addColumn<bool>("contextAvailable");
    QTest::addColumn<bool>("cacheAvailable");
    QTest::addColumn<bool>("sourceAvailable");
    QTest::addColumn<bool>("sourceInCache");

    QTest::newRow("context-destroyed")
        << false << true << true << true;
    QTest::newRow("cache-destroyed")
        << true << false << true << true;
    QTest::newRow("source-destroyed")
        << true << true << false << true;
    QTest::newRow("source-replaced")
        << true << true << true << false;
}

void TestSplitRideData::
sourceIdentityRequiresLiveWorkflowOwners()
{
    QFETCH(bool, contextAvailable);
    QFETCH(bool, cacheAvailable);
    QFETCH(bool, sourceAvailable);
    QFETCH(bool, sourceInCache);
    const SplitActivitySourceIdentity identity{
        QStringLiteral("source.json"),
        QStringLiteral("/activities"),
        false};

    QVERIFY(!splitActivitySourceIsCurrent(
        contextAvailable, cacheAvailable,
        sourceAvailable, sourceInCache,
        identity, identity));
}

void TestSplitRideData::sourceIdentityRejectsMutation_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("planned");

    QTest::newRow("filename")
        << QStringLiteral("replacement.json")
        << QStringLiteral("/activities") << false;
    QTest::newRow("path")
        << QStringLiteral("source.json")
        << QStringLiteral("/temporary") << false;
    QTest::newRow("namespace")
        << QStringLiteral("source.json")
        << QStringLiteral("/activities") << true;
}

void TestSplitRideData::sourceIdentityRejectsMutation()
{
    QFETCH(QString, fileName);
    QFETCH(QString, path);
    QFETCH(bool, planned);
    const SplitActivitySourceIdentity expected{
        QStringLiteral("source.json"),
        QStringLiteral("/activities"),
        false};
    const SplitActivitySourceIdentity current{
        fileName, path, planned};

    QVERIFY(!splitActivitySourceIsCurrent(
        true, true, true, true,
        expected, current));
}

void TestSplitRideData::
postPublishRevalidationStopsAfterRecoveryDialog()
{
    const SplitActivitySourceIdentity expected{
        QStringLiteral("source.json"),
        QStringLiteral("/activities"),
        false};
    SplitActivitySourceIdentity current = expected;
    bool cleanupRan = false;

    QVERIFY(splitActivitySourceIsCurrent(
        true, true, true, true,
        expected, current));

    const auto recoveryDialog = [&] {
        current.path = QStringLiteral("/temporary");
    };
    recoveryDialog();
    if (splitActivitySourceIsCurrent(
            true, true, true, true,
            expected, current)) {
        cleanupRan = true;
    }

    QVERIFY(!cleanupRan);
}

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
