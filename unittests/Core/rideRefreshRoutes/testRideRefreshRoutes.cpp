#include <QtTest>

#include "RideRefreshRoutes.h"

namespace {

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

QTEST_APPLESS_MAIN(TestRideRefreshRoutes)

#include "testRideRefreshRoutes.moc"
