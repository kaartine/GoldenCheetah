/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <type_traits>

#include "Voronoi.h"

static bool hasOnlyFiniteLines(const QList<QLineF> &lines)
{
    for (const QLineF &line : lines) {
        if (!std::isfinite(line.x1())
            || !std::isfinite(line.y1())
            || !std::isfinite(line.x2())
            || !std::isfinite(line.y2())) {
            return false;
        }
    }
    return true;
}

class TestVoronoiSafety : public QObject
{
    Q_OBJECT

private slots:
    void constructorInitializesAccounting();
    void ownershipIsUnique();
    void rejectsUnsafeSites_data();
    void rejectsUnsafeSites();
    void deduplicatesStoredCoordinates();
    void largeFiniteIntersectionRemainsRepresentable();
    void rejectsUnrepresentableIntersection();
    void largeFiniteDistanceRemainsFinite();
    void largeRightOfPredicateRemainsStable();
    void largeRightOfSubtractionUsesDouble();
    void rejectsUnsafeQueuePriority();
    void lineRejectsNonFiniteCoordinates();
    void rejectsInsufficientSites();
    void rejectsUnrepresentableBisector();
    void collinearSitesRemainFinite();
    void verticalCollinearSitesRemainFinite();
    void rejectsUnsafePlotBounds();
    void largeFiniteDiagramRemainsFinite();
    void randomizedFiniteSitesRemainFinite();
    void rejectedSitesCannotContaminateDiagram();
    void rejectsReuseAfterSweep();
    void rejectsReuseAfterFailedSweep();
};

void TestVoronoiSafety::constructorInitializesAccounting()
{
    alignas(Voronoi) unsigned char storage[sizeof(Voronoi)];
    std::memset(storage, 0xa5, sizeof(storage));

    Voronoi *voronoi = new (storage) Voronoi;
    const int siteIndex = voronoi->siteidx;
    const int searchAttempts = voronoi->ntry;
    const int totalSearch = voronoi->totalsearch;
    const int allocatedBytes = voronoi->total_alloc;
    voronoi->~Voronoi();

    QCOMPARE(siteIndex, 0);
    QCOMPARE(searchAttempts, 0);
    QCOMPARE(totalSearch, 0);
    QCOMPARE(allocatedBytes, 0);
}

void TestVoronoiSafety::ownershipIsUnique()
{
    QVERIFY(!std::is_copy_constructible<Voronoi>::value);
    QVERIFY(!std::is_copy_assignable<Voronoi>::value);
}

void TestVoronoiSafety::rejectsUnsafeSites_data()
{
    QTest::addColumn<double>("x");
    QTest::addColumn<double>("y");

    const double infinity = std::numeric_limits<double>::infinity();
    QTest::newRow("nan") << std::numeric_limits<double>::quiet_NaN() << 0.0;
    QTest::newRow("positive-infinity") << infinity << 0.0;
    QTest::newRow("negative-infinity") << 0.0 << -infinity;
    QTest::newRow("positive-float-overflow")
        << std::numeric_limits<double>::max() << 0.0;
    QTest::newRow("negative-float-overflow")
        << -std::numeric_limits<double>::max() << 0.0;
}

void TestVoronoiSafety::rejectsUnsafeSites()
{
    QFETCH(double, x);
    QFETCH(double, y);

    Voronoi voronoi;
    QVERIFY(!voronoi.addSite(QPointF(x, y)));
    QVERIFY(!voronoi.run(QRectF()));
    QVERIFY(voronoi.lines().isEmpty());
}

void TestVoronoiSafety::deduplicatesStoredCoordinates()
{
    Voronoi voronoi;
    QVERIFY(voronoi.addSite(QPointF(1.0, 1.0)));
    QVERIFY(!voronoi.addSite(QPointF(1.0, 1.0)));
    QVERIFY(!voronoi.addSite(
        QPointF(std::nextafter(1.0, 2.0), 1.0)));
    QVERIFY(voronoi.addSite(QPointF(2.0, 1.0)));

    QVERIFY(voronoi.run(QRectF()));
    QVERIFY(hasOnlyFiniteLines(voronoi.lines()));
}

void TestVoronoiSafety::largeFiniteIntersectionRemainsRepresentable()
{
    const float maximum = std::numeric_limits<float>::max();
    Site firstRegion{};
    Site secondRegion{};
    firstRegion.coord = {0.0f, 0.0f};
    secondRegion.coord = {0.0f, 1.0f};

    Edge firstEdge{};
    firstEdge.a = 1.0f;
    firstEdge.b = 1.0f;
    firstEdge.c = maximum;
    firstEdge.reg[1] = &firstRegion;

    Edge secondEdge{};
    secondEdge.a = -1.0f;
    secondEdge.b = 1.0f;
    secondEdge.c = -maximum;
    secondEdge.reg[1] = &secondRegion;

    Halfedge firstHalfedge{};
    firstHalfedge.ELedge = &firstEdge;
    firstHalfedge.ELpm = voronoi_re;

    Halfedge secondHalfedge{};
    secondHalfedge.ELedge = &secondEdge;
    secondHalfedge.ELpm = voronoi_re;

    Voronoi voronoi;
    const Site *intersection =
        voronoi.intersect(&firstHalfedge, &secondHalfedge);
    QVERIFY(intersection);
    QCOMPARE(intersection->coord.x, maximum);
    QCOMPARE(intersection->coord.y, 0.0f);
}

void TestVoronoiSafety::rejectsUnrepresentableIntersection()
{
    const float maximum = std::numeric_limits<float>::max();
    Site firstRegion{};
    Site secondRegion{};
    firstRegion.coord = {0.0f, 0.0f};
    secondRegion.coord = {0.0f, 1.0f};

    Edge firstEdge{};
    firstEdge.a = 1.0f;
    firstEdge.b = 1.0f;
    firstEdge.c = maximum;
    firstEdge.reg[1] = &firstRegion;

    Edge secondEdge{};
    secondEdge.a = 0.0f;
    secondEdge.b = 1.0f;
    secondEdge.c = -maximum;
    secondEdge.reg[1] = &secondRegion;

    Halfedge firstHalfedge{};
    firstHalfedge.ELedge = &firstEdge;
    firstHalfedge.ELpm = voronoi_re;

    Halfedge secondHalfedge{};
    secondHalfedge.ELedge = &secondEdge;
    secondHalfedge.ELpm = voronoi_re;

    Voronoi voronoi;
    QVERIFY(!voronoi.intersect(&firstHalfedge, &secondHalfedge));
}

void TestVoronoiSafety::largeFiniteDistanceRemainsFinite()
{
    Site first{};
    Site second{};
    first.coord = {0.0f, 0.0f};
    second.coord = {1.0e20f, 1.0e20f};

    Voronoi voronoi;
    const double distance = voronoi.dist(&first, &second);
    QVERIFY(std::isfinite(distance));
    QVERIFY(qFuzzyCompare(
        distance,
        std::hypot(
            static_cast<double>(second.coord.x),
            static_cast<double>(second.coord.y))));
}

void TestVoronoiSafety::largeRightOfPredicateRemainsStable()
{
    const auto predicate = [](float scale) {
        Site bottom{};
        Site top{};
        bottom.coord = {0.0f, 0.0f};
        top.coord = {scale, 0.0f};

        Edge edge{};
        edge.a = 1.0f;
        edge.b = 0.5f;
        edge.c = 0.9f * scale;
        edge.reg[0] = &bottom;
        edge.reg[1] = &top;

        Halfedge halfedge{};
        halfedge.ELedge = &edge;
        halfedge.ELpm = voronoi_le;

        Point point{0.5f * scale, scale};
        Voronoi voronoi;
        return voronoi.right_of(&halfedge, &point);
    };

    QCOMPARE(predicate(1.0f), 1);
    QCOMPARE(predicate(1.0e20f), 1);
}

void TestVoronoiSafety::largeRightOfSubtractionUsesDouble()
{
    Site bottom{};
    Site top{};
    bottom.coord = {-0x1.e66664p+127f, -0x1.ccccccp+127f};
    top.coord = {-0x1.999998p+127f, -0x1.ccccccp+126f};

    Edge edge{};
    edge.a = 0x1.555552p-2f;
    edge.b = 0x1p+0f;
    edge.c = -0x1.eeeeecp+127f;
    edge.reg[0] = &bottom;
    edge.reg[1] = &top;

    Halfedge halfedge{};
    halfedge.ELedge = &edge;
    halfedge.ELpm = voronoi_re;
    Point point{-0x1.999998p+125f, -0x1.333332p+125f};
    QVERIFY(point.x > top.coord.x);

    const double lineY =
        static_cast<double>(edge.c)
        - static_cast<double>(edge.a) * point.x;
    const double verticalDistance =
        static_cast<double>(point.y) - lineY;
    const double horizontalDistance =
        static_cast<double>(point.x) - top.coord.x;
    const double topDistance =
        lineY - static_cast<double>(top.coord.y);
    const int above =
        verticalDistance * verticalDistance
            > horizontalDistance * horizontalDistance
                + topDistance * topDistance;
    const int expected = !above;
    QCOMPARE(expected, 0);

    Voronoi voronoi;
    QCOMPARE(voronoi.right_of(&halfedge, &point), expected);
}

void TestVoronoiSafety::rejectsUnsafeQueuePriority()
{
    Halfedge halfedge{};
    Site site{};
    site.coord = {0.0f, std::numeric_limits<float>::max()};

    Voronoi voronoi;
    QVERIFY(!voronoi.PQinsert(
        &halfedge, &site, std::numeric_limits<float>::max()));
    QCOMPARE(site.refcnt, 0);
    QVERIFY(!halfedge.vertex);
    QCOMPARE(voronoi.PQcount, 0);
}

void TestVoronoiSafety::lineRejectsNonFiniteCoordinates()
{
    Voronoi voronoi;
    voronoi.line(0.0, 0.0, 1.0, 1.0);
    voronoi.line(
        std::numeric_limits<double>::quiet_NaN(),
        0.0,
        1.0,
        1.0);

    QCOMPARE(voronoi.lines().count(), 1);
    QVERIFY(hasOnlyFiniteLines(voronoi.lines()));
}

void TestVoronoiSafety::rejectsInsufficientSites()
{
    Voronoi voronoi;
    QVERIFY(voronoi.addSite(QPointF(1.0, 1.0)));
    QVERIFY(!voronoi.run(QRectF()));
    QVERIFY(voronoi.lines().isEmpty());
    QVERIFY(voronoi.addSite(QPointF(2.0, 1.0)));
    QVERIFY(voronoi.run(QRectF()));
    QVERIFY(hasOnlyFiniteLines(voronoi.lines()));
}

void TestVoronoiSafety::rejectsUnrepresentableBisector()
{
    const double maximum = std::numeric_limits<float>::max();

    Voronoi voronoi;
    QVERIFY(voronoi.addSite(
        QPointF(maximum * 0.59, maximum * 0.59)));
    QVERIFY(voronoi.addSite(
        QPointF(maximum * 0.61, maximum * 0.61)));
    QVERIFY(!voronoi.run(QRectF()));
    QVERIFY(voronoi.lines().isEmpty());
}

void TestVoronoiSafety::collinearSitesRemainFinite()
{
    Voronoi voronoi;
    for (int i = 0; i < 5; ++i) {
        QVERIFY(voronoi.addSite(QPointF(i, 0.0)));
    }

    QVERIFY(voronoi.run(QRectF()));
    QVERIFY(!voronoi.lines().isEmpty());
    QVERIFY(hasOnlyFiniteLines(voronoi.lines()));
}

void TestVoronoiSafety::verticalCollinearSitesRemainFinite()
{
    Voronoi voronoi;
    for (int i = 0; i < 5; ++i) {
        QVERIFY(voronoi.addSite(QPointF(0.0, i)));
    }

    QVERIFY(voronoi.run(QRectF()));
    QVERIFY(!voronoi.lines().isEmpty());
    QVERIFY(hasOnlyFiniteLines(voronoi.lines()));
}

void TestVoronoiSafety::rejectsUnsafePlotBounds()
{
    const double maximum = std::numeric_limits<float>::max();

    Voronoi voronoi;
    QVERIFY(voronoi.addSite(QPointF(maximum * 0.95, maximum * -0.1)));
    QVERIFY(voronoi.addSite(QPointF(maximum, maximum * 0.1)));
    QVERIFY(!voronoi.run(QRectF()));
    QVERIFY(voronoi.lines().isEmpty());
}

void TestVoronoiSafety::largeFiniteDiagramRemainsFinite()
{
    Voronoi voronoi;
    QVERIFY(voronoi.addSite(QPointF(0.0, 0.0)));
    QVERIFY(voronoi.addSite(QPointF(1.0e20, 0.0)));
    QVERIFY(voronoi.addSite(QPointF(0.0, 1.0e20)));
    QVERIFY(voronoi.addSite(QPointF(1.0e20, 1.0e20)));

    QVERIFY(voronoi.run(QRectF()));
    QVERIFY(!voronoi.lines().isEmpty());
    QVERIFY(hasOnlyFiniteLines(voronoi.lines()));
}

void TestVoronoiSafety::randomizedFiniteSitesRemainFinite()
{
    std::mt19937_64 generator(0x6f6e6f69);
    std::uniform_real_distribution<double> coordinate(-1000.0, 1000.0);

    for (int round = 0; round < 25; ++round) {
        Voronoi voronoi;
        for (int site = 0; site < 40; ++site) {
            QVERIFY(voronoi.addSite(
                QPointF(coordinate(generator), coordinate(generator))));
        }

        QVERIFY(voronoi.run(QRectF()));
        QVERIFY(hasOnlyFiniteLines(voronoi.lines()));
    }
}

void TestVoronoiSafety::rejectedSitesCannotContaminateDiagram()
{
    Voronoi voronoi;
    QVERIFY(voronoi.addSite(QPointF(0.0, 0.0)));
    QVERIFY(voronoi.addSite(QPointF(2.0, 0.0)));
    QVERIFY(voronoi.addSite(QPointF(1.0, 2.0)));
    QVERIFY(!voronoi.addSite(QPointF(
        std::numeric_limits<double>::quiet_NaN(), 1.0)));
    QVERIFY(!voronoi.addSite(QPointF(1.0, 2.0)));

    QVERIFY(voronoi.run(QRectF()));
    QVERIFY(!voronoi.lines().isEmpty());
    QVERIFY(hasOnlyFiniteLines(voronoi.lines()));
}

void TestVoronoiSafety::rejectsReuseAfterSweep()
{
    Voronoi voronoi;
    QVERIFY(voronoi.addSite(QPointF(0.0, 0.0)));
    QVERIFY(voronoi.addSite(QPointF(2.0, 0.0)));
    QVERIFY(voronoi.addSite(QPointF(1.0, 2.0)));
    QVERIFY(voronoi.run(QRectF()));

    QVERIFY(!voronoi.addSite(QPointF(3.0, 3.0)));
    QVERIFY(!voronoi.run(QRectF()));
    QVERIFY(voronoi.lines().isEmpty());
}

void TestVoronoiSafety::rejectsReuseAfterFailedSweep()
{
    const double maximum = std::numeric_limits<float>::max();
    Voronoi voronoi;
    QVERIFY(voronoi.addSite(QPointF(
        maximum * 0.95, maximum * -0.1)));
    QVERIFY(voronoi.addSite(QPointF(
        maximum, maximum * 0.1)));
    QVERIFY(!voronoi.run(QRectF()));

    QVERIFY(!voronoi.addSite(QPointF(0.0, 0.0)));
    QVERIFY(!voronoi.run(QRectF()));
    QVERIFY(voronoi.lines().isEmpty());
}

QTEST_APPLESS_MAIN(TestVoronoiSafety)

#include "testVoronoiSafety.moc"
