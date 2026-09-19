#include <QtTest>

#include "RideRefreshRoutes.h"

#include <QDir>
#include <QFile>
#include <QThread>

namespace {

#define GC_STRINGIFY_IMPL(value) #value
#define GC_STRINGIFY(value) GC_STRINGIFY_IMPL(value)

RideRefreshRoutes::Segment segment(
    const QUuid &id,
    const QString &name,
    QVector<RideRefreshRoutes::Point> points)
{
    RideRefreshRoutes::Segment result;
    result.id = id;
    result.name = name;
    result.points = std::move(points);
    for (const auto &point : result.points) {
        result.minimumLatitude =
            qMin(result.minimumLatitude, point.latitude);
        result.maximumLatitude =
            qMax(result.maximumLatitude, point.latitude);
        result.minimumLongitude =
            qMin(result.minimumLongitude, point.longitude);
        result.maximumLongitude =
            qMax(result.maximumLongitude, point.longitude);
    }
    return result;
}

RideRefreshRoutes::Ride ride(
    QVector<RideRefreshRoutes::RidePoint> points)
{
    RideRefreshRoutes::Ride result;
    result.points = std::move(points);
    result.minimumLatitude = 180.0;
    result.maximumLatitude = -180.0;
    result.minimumLongitude = 180.0;
    result.maximumLongitude = -180.0;
    for (const auto &point : result.points) {
        result.minimumLatitude =
            qMin(result.minimumLatitude, point.latitude);
        result.maximumLatitude =
            qMax(result.maximumLatitude, point.latitude);
        result.minimumLongitude =
            qMin(result.minimumLongitude, point.longitude);
        result.maximumLongitude =
            qMax(result.maximumLongitude, point.longitude);
    }
    return result;
}

} // namespace

class TestRideRefreshRoutes : public QObject
{
    Q_OBJECT

private slots:
    void snapshotOwnsGeometryAndFingerprint();
    void exactRouteReturnsValueOnlyMatch();
    void boundsAndInvalidGpsFailClosed();
    void outerAndLookAheadGpsValidityMatchLegacyQuirks();
    void distanceLookupUsesLegacyLowerBoundAndBounds();
    void searchPreservesRouteOrderAndStrictBoundsTolerance();
    void farPointSkipAndLookAheadMatchLegacyScanning();
    void divergenceRestartsFromTheFirstRoutePoint();
    void parametersControlRestartAndOccurrenceNumbering();
    void ownerThreadAssemblyPreservesCompleteOrderedValues();
    void productionCaptureChecksOwnerBeforeReadingRoutes();
};

void TestRideRefreshRoutes::snapshotOwnsGeometryAndFingerprint()
{
    QVector<RideRefreshRoutes::Segment> input = {
        segment(QUuid::createUuid(), QStringLiteral("first"), {{24.0, 60.0}}),
        segment(QUuid::createUuid(), QStringLiteral("second"), {{25.0, 61.0}})
    };
    const QUuid firstId = input[0].id;
    const auto snapshot = RideRefreshRoutes::create(input, 0x5a3c);
    input[0].id = QUuid::createUuid();
    input[0].points[0].latitude = -90.0;

    QCOMPARE(snapshot->fingerprint(), quint16(0x5a3c));
    QCOMPARE(snapshot->segments().size(), 2);
    QCOMPARE(snapshot->segments()[0].id, firstId);
    QCOMPARE(snapshot->segments()[0].points[0].latitude, 60.0);
    QCOMPARE(snapshot->segments()[1].name, QStringLiteral("second"));
}

void TestRideRefreshRoutes::exactRouteReturnsValueOnlyMatch()
{
    const QUuid id = QUuid::createUuid();
    const auto snapshot = RideRefreshRoutes::create({segment(
        id, QStringLiteral("commute"), {{24.0, 60.0}, {24.001, 60.001}})},
        17);
    const auto input = ride({
        {10.0, 1.0, 24.0, 60.0},
        {20.0, 2.0, 24.0005, 60.0005},
        {30.0, 3.0, 24.001, 60.001}
    });

    const QVector<RideRefreshRoutes::Match> matches = snapshot->search(input);
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches[0].routeId, id);
    QCOMPARE(matches[0].name, QStringLiteral("commute"));
    QCOMPARE(matches[0].startSeconds, 10.0);
    QCOMPARE(matches[0].stopSeconds, 30.0);
    QCOMPARE(matches[0].startKilometres, 1.0);
    QCOMPARE(matches[0].stopKilometres, 3.0);
    QCOMPARE(matches[0].occurrence, 1);
}

void TestRideRefreshRoutes::boundsAndInvalidGpsFailClosed()
{
    const auto snapshot = RideRefreshRoutes::create({segment(
        QUuid::createUuid(), QStringLiteral("route"), {{24.0, 60.0}})}, 1);
    auto outside = ride({{10.0, 1.0, 25.0, 61.0}});
    QVERIFY(snapshot->search(outside).isEmpty());

    auto invalid = ride({{10.0, 1.0, 0.0, 0.0}});
    invalid.minimumLatitude = 59.0;
    invalid.maximumLatitude = 61.0;
    invalid.minimumLongitude = 23.0;
    invalid.maximumLongitude = 25.0;
    QVERIFY(snapshot->search(invalid).isEmpty());
}

void TestRideRefreshRoutes::outerAndLookAheadGpsValidityMatchLegacyQuirks()
{
    const auto rejectedOuter540 = RideRefreshRoutes::create({segment(
        QUuid::createUuid(), QStringLiteral("outer-540"), {{539.1, 60.0}})},
        1);
    auto outer540 = ride({{10.0, 1.0, 539.1, 60.0}});
    QVERIFY(rejectedOuter540->search(outer540).isEmpty());

    const auto acceptedLookAhead540 = RideRefreshRoutes::create({segment(
        QUuid::createUuid(), QStringLiteral("lookahead-540"),
        {{24.0, 60.0}, {539.1, 60.0}})}, 1);
    auto lookAhead540 = ride({
        {10.0, 1.0, 24.0, 60.0},
        {20.0, 2.0, 25.0, 60.0},
        {30.0, 3.0, 539.1, 60.0}
    });
    QCOMPARE(acceptedLookAhead540->search(lookAhead540).size(), 1);

    const auto rejectedLookAhead180 = RideRefreshRoutes::create({segment(
        QUuid::createUuid(), QStringLiteral("lookahead-180"),
        {{24.0, 60.0}, {179.1, 60.0}})}, 1);
    auto lookAhead180 = ride({
        {10.0, 1.0, 24.0, 60.0},
        {20.0, 2.0, 25.0, 60.0},
        {30.0, 3.0, 179.1, 60.0}
    });
    QVERIFY(rejectedLookAhead180->search(lookAhead180).isEmpty());
}

void TestRideRefreshRoutes::distanceLookupUsesLegacyLowerBoundAndBounds()
{
    const RideRefreshRoutes::Ride input = ride({
        {10.0, 1.0, 24.0, 60.0},
        {20.0, 2.0, 24.1, 60.1},
        {30.0, 3.0, 24.2, 60.2}
    });
    QCOMPARE(RideRefreshRoutes::distanceAt(input, 5.0), 1.0);
    QCOMPARE(RideRefreshRoutes::distanceAt(input, 10.0), 1.0);
    QCOMPARE(RideRefreshRoutes::distanceAt(input, 15.0), 2.0);
    QCOMPARE(RideRefreshRoutes::distanceAt(input, 30.0), 3.0);
    QCOMPARE(RideRefreshRoutes::distanceAt(input, 35.0), 3.0);
    QCOMPARE(RideRefreshRoutes::distanceAt({}, 15.0), 0.0);
}

void TestRideRefreshRoutes::
searchPreservesRouteOrderAndStrictBoundsTolerance()
{
    const QUuid first = QUuid::createUuid();
    const QUuid second = QUuid::createUuid();
    const auto snapshot = RideRefreshRoutes::create({
        segment(first, QStringLiteral("first"), {{24.0, 60.0}}),
        segment(second, QStringLiteral("second"), {{24.0, 60.0}})
    }, 1);
    const auto input = ride({{10.0, 1.0, 24.0, 60.0}});
    const auto matches = snapshot->search(input);
    QCOMPARE(matches.size(), 2);
    QCOMPARE(matches[0].routeId, first);
    QCOMPARE(matches[1].routeId, second);

    RideRefreshRoutes::Ride exactBoundary = input;
    exactBoundary.minimumLatitude = 60.001;
    QVERIFY(snapshot->search(exactBoundary).isEmpty());
}

void TestRideRefreshRoutes::farPointSkipAndLookAheadMatchLegacyScanning()
{
    RideRefreshRoutes::SearchParameters parameters;
    parameters.restartSkip = 100;
    const auto snapshot = RideRefreshRoutes::create({segment(
        QUuid::createUuid(), QStringLiteral("scan"), {{24.0, 60.0}})},
        1, parameters);

    QVector<RideRefreshRoutes::RidePoint> samples;
    samples.append({0.0, 0.0, 10.0, 10.0});
    for (int index = 1; index < 50; ++index)
        samples.append({double(index), index / 10.0, 10.0, 10.0});
    // Legacy i += 50 followed by the loop increment skips index 50.
    samples.append({50.0, 5.0, 24.0, 60.0});
    samples.append({51.0, 5.1, 24.0004, 60.0004});
    samples.append({52.0, 5.2, 24.0, 60.0});
    auto input = ride(std::move(samples));
    input.minimumLatitude = 10.0;
    input.maximumLatitude = 60.0;
    input.minimumLongitude = 10.0;
    input.maximumLongitude = 24.0;

    const auto matches = snapshot->search(input);
    QCOMPARE(matches.size(), 1);
    // Look-ahead selects the exact sample at 52 rather than the first
    // in-range sample at 51; time-to-distance uses the same lower bound.
    QCOMPARE(matches[0].startSeconds, 52.0);
    QCOMPARE(matches[0].startKilometres, 5.2);
}

void TestRideRefreshRoutes::divergenceRestartsFromTheFirstRoutePoint()
{
    RideRefreshRoutes::SearchParameters parameters;
    parameters.restartSkip = 1;
    const auto snapshot = RideRefreshRoutes::create({segment(
        QUuid::createUuid(), QStringLiteral("restart"),
        {{24.0, 60.0}, {24.001, 60.001}})}, 1, parameters);
    QVector<RideRefreshRoutes::RidePoint> samples = {
        {10.0, 1.0, 24.0, 60.0},
        {11.0, 1.1, 24.0001, 60.0001}
    };
    // Keep the later complete occurrence beyond the ten-sample look-ahead so
    // three valid-but-diverged scans reset the partial first attempt.
    for (int index = 0; index < 15; ++index) {
        samples.append({20.0 + index, 2.0 + index / 10.0,
                        24.006, 60.006});
    }
    samples.append({40.0, 4.0, 24.0, 60.0});
    samples.append({50.0, 5.0, 24.001, 60.001});
    const auto input = ride(std::move(samples));

    const auto matches = snapshot->search(input);
    QCOMPARE(matches.size(), 2);
    // Legacy search publishes the final-point attempt even when divergence
    // just requested a restart. Preserve it until the behavior is migrated.
    QCOMPARE(matches[0].startSeconds, 10.0);
    QCOMPARE(matches[0].stopSeconds, 21.0);
    QCOMPARE(matches[1].startSeconds, 40.0);
    QCOMPARE(matches[1].stopSeconds, 50.0);
}

void TestRideRefreshRoutes::parametersControlRestartAndOccurrenceNumbering()
{
    RideRefreshRoutes::SearchParameters parameters;
    parameters.restartSkip = 1;
    const auto snapshot = RideRefreshRoutes::create({segment(
        QUuid::createUuid(), QStringLiteral("repeat"), {{24.0, 60.0}})},
        1, parameters);
    const auto input = ride({
        {10.0, 1.0, 24.0, 60.0},
        {20.0, 2.0, 25.0, 61.0},
        {30.0, 3.0, 24.0, 60.0}
    });

    const auto matches = snapshot->search(input);
    QCOMPARE(matches.size(), 2);
    QCOMPARE(matches[0].occurrence, 1);
    QCOMPARE(matches[0].startSeconds, 10.0);
    QCOMPARE(matches[1].occurrence, 2);
    QCOMPARE(matches[1].startSeconds, 30.0);
}

void TestRideRefreshRoutes::ownerThreadAssemblyPreservesCompleteOrderedValues()
{
    QObject owner;
    const QUuid first = QUuid::createUuid();
    const QUuid second = QUuid::createUuid();
    RideRefreshRoutes::Segment firstSegment;
    firstSegment.id = first;
    firstSegment.name = QStringLiteral("first");
    firstSegment.points = {{24.25, 60.75}, {24.5, 61.0}};
    firstSegment.minimumLatitude = 59.5;
    firstSegment.maximumLatitude = 61.25;
    firstSegment.minimumLongitude = 23.5;
    firstSegment.maximumLongitude = 25.75;
    RideRefreshRoutes::Segment secondSegment;
    secondSegment.id = second;
    secondSegment.name = QStringLiteral("second");
    secondSegment.points = {{-3.25, 52.125}, {-2.75, 52.625}};
    secondSegment.minimumLatitude = 51.5;
    secondSegment.maximumLatitude = 53.25;
    secondSegment.minimumLongitude = -4.5;
    secondSegment.maximumLongitude = -1.75;
    QVector<RideRefreshRoutes::Segment> segments = {
        firstSegment, secondSegment};
    const auto captured = captureRideRefreshRoutesForOwner(
        &owner, segments, 0x4321);
    segments[0].points[0] = {-99.0, -88.0};
    segments[1].name = QStringLiteral("mutated");
    QVERIFY(captured);
    QCOMPARE(captured->fingerprint(), quint16(0x4321));
    QCOMPARE(captured->segments().size(), 2);
    QCOMPARE(captured->segments()[0].id, first);
    QCOMPARE(captured->segments()[0].name, QStringLiteral("first"));
    QCOMPARE(captured->segments()[0].points.size(), 2);
    QCOMPARE(captured->segments()[0].points[0].longitude, 24.25);
    QCOMPARE(captured->segments()[0].points[0].latitude, 60.75);
    QCOMPARE(captured->segments()[0].points[1].longitude, 24.5);
    QCOMPARE(captured->segments()[0].points[1].latitude, 61.0);
    QCOMPARE(captured->segments()[0].minimumLatitude, 59.5);
    QCOMPARE(captured->segments()[0].maximumLatitude, 61.25);
    QCOMPARE(captured->segments()[0].minimumLongitude, 23.5);
    QCOMPARE(captured->segments()[0].maximumLongitude, 25.75);
    QCOMPARE(captured->segments()[1].id, second);
    QCOMPARE(captured->segments()[1].name, QStringLiteral("second"));
    QCOMPARE(captured->segments()[1].points.size(), 2);
    QCOMPARE(captured->segments()[1].points[0].longitude, -3.25);
    QCOMPARE(captured->segments()[1].points[0].latitude, 52.125);
    QCOMPARE(captured->segments()[1].points[1].longitude, -2.75);
    QCOMPARE(captured->segments()[1].points[1].latitude, 52.625);
    QCOMPARE(captured->segments()[1].minimumLatitude, 51.5);
    QCOMPARE(captured->segments()[1].maximumLatitude, 53.25);
    QCOMPARE(captured->segments()[1].minimumLongitude, -4.5);
    QCOMPARE(captured->segments()[1].maximumLongitude, -1.75);
    QVERIFY(!captureRideRefreshRoutesForOwner(nullptr, segments, 1));

    QThread foreignThread;
    QObject foreignOwner;
    foreignOwner.moveToThread(&foreignThread);
    foreignThread.start();
    QVERIFY(!captureRideRefreshRoutesForOwner(
        &foreignOwner, segments, 1));
    QThread *testThread = QThread::currentThread();
    QMetaObject::invokeMethod(
        &foreignOwner,
        [&foreignOwner, testThread]() {
            foreignOwner.moveToThread(testThread);
        },
        Qt::BlockingQueuedConnection);
    foreignThread.quit();
    QVERIFY(foreignThread.wait());
}

void TestRideRefreshRoutes::productionCaptureChecksOwnerBeforeReadingRoutes()
{
    QFile file(QDir(QString::fromUtf8(GC_STRINGIFY(GC_TEST_SOURCE_ROOT)))
                   .filePath(QStringLiteral(
                       "src/Core/RideRefreshRoutesCapture.cpp")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray source = file.readAll();
    const qsizetype function = source.indexOf("captureRideRefreshRoutes(");
    const qsizetype nullAndOwnerGuard = source.indexOf(
        "if (!source || QThread::currentThread() != source->thread())",
        function);
    const qsizetype ownerCheck = source.indexOf(
        "QThread::currentThread() != source->thread()", function);
    const qsizetype orderedTraversal = source.indexOf(
        "for (const RouteSegment &live : source->routes)", function);
    const qsizetype firstRouteRead = source.indexOf(
        "source->routes", function);
    const qsizetype orderedAppend = source.indexOf(
        "segments.append(std::move(segment));", function);
    const qsizetype fingerprintRead = source.indexOf(
        "source->getFingerprint()", function);
    const qsizetype exactDelegation = source.indexOf(
        "source, std::move(segments), source->getFingerprint()", function);

    QVERIFY(function >= 0);
    QVERIFY(nullAndOwnerGuard > function);
    QVERIFY(ownerCheck > function);
    QVERIFY(firstRouteRead > ownerCheck);
    QVERIFY(orderedTraversal > ownerCheck);
    QVERIFY(orderedAppend > orderedTraversal);
    QVERIFY(fingerprintRead > orderedAppend);
    QVERIFY(exactDelegation > orderedAppend);
    for (const QByteArray &mapping : {
             QByteArray("segment.id = live._id;"),
             QByteArray("segment.name = live.name;"),
             QByteArray("segment.minimumLatitude = live.minLat;"),
             QByteArray("segment.maximumLatitude = live.maxLat;"),
             QByteArray("segment.minimumLongitude = live.minLon;"),
             QByteArray("segment.maximumLongitude = live.maxLon;"),
             QByteArray("segment.points.reserve(live.points.size());"),
             QByteArray("for (const RoutePoint &point : live.points)"),
             QByteArray("segment.points.append({point.lon, point.lat});")}) {
        const qsizetype mappingPosition = source.indexOf(mapping, function);
        QVERIFY2(mappingPosition > ownerCheck, mapping.constData());
    }
}

QTEST_GUILESS_MAIN(TestRideRefreshRoutes)

#include "testRideRefreshRoutes.moc"
