#include <QtTest>

#include "RideCacheStartup.h"

#include <algorithm>

class TestRideCachePerformance : public QObject
{
    Q_OBJECT

private slots:
    void startupIndex_data();
    void startupIndex();
    void startupBatchesAreBounded();
    void snapshotApplicationRequiresUntouchedTarget();
    void invalidationScope_data();
    void invalidationScope();
    void refreshGenerationsRejectSupersededWork();
    void refreshCancellationSettlesPendingWork();
};

void TestRideCachePerformance::startupIndex_data()
{
    QTest::addColumn<int>("rideCount");

    QTest::newRow("1k activities") << 1000;
    QTest::newRow("10k activities") << 10000;
    QTest::newRow("50k activities") << 50000;
}

void TestRideCachePerformance::startupIndex()
{
    QFETCH(int, rideCount);

    const QDateTime base(
        QDate(2020, 1, 1), QTime(0, 0), QTimeZone::UTC);
    QStringList activities;
    QStringList planned;
    activities.reserve((rideCount + 1) / 2);
    planned.reserve(rideCount / 2);

    for (int index = rideCount - 1; index >= 0; --index) {
        const QString name =
            QStringLiteral("%1.json").arg(
                index, 8, 10, QLatin1Char('0'));
        if (index % 2) planned.append(name);
        else activities.append(name);
    }

    int parserCalls = 0;
    const auto parser = [&](const QString &name, QDateTime *dateTime) {
        ++parserCalls;
        bool valid = false;
        const qint64 seconds = name.first(8).toLongLong(&valid);
        if (valid) *dateTime = base.addSecs(seconds);
        return valid;
    };

    QElapsedTimer timer;
    timer.start();
    const QVector<RideCacheStartup::IndexedFile> cold =
        RideCacheStartup::buildIndex(
            activities, QStringLiteral("/activities"),
            planned, QStringLiteral("/planned"), parser);
    const qint64 coldMs = timer.elapsed();

    timer.restart();
    const QVector<RideCacheStartup::IndexedFile> warm =
        RideCacheStartup::buildIndex(
            activities, QStringLiteral("/activities"),
            planned, QStringLiteral("/planned"), parser);
    const qint64 warmMs = timer.elapsed();

    QCOMPARE(cold.size(), rideCount);
    QCOMPARE(warm.size(), rideCount);
    QCOMPARE(parserCalls, rideCount * 2);
    QVERIFY(std::is_sorted(
        cold.cbegin(), cold.cend(),
        [](const auto &left, const auto &right) {
            return left.dateTime < right.dateTime;
        }));
    QCOMPARE(cold.constFirst().dateTime, base);
    QCOMPARE(
        cold.constLast().dateTime, base.addSecs(rideCount - 1));
    QCOMPARE(cold.constFirst().planned, false);
    QCOMPARE(
        cold.constLast().planned, (rideCount - 1) % 2 != 0);
    QVERIFY2(
        coldMs < 10000, "cold startup index exceeded 10 seconds");
    QVERIFY2(
        warmMs < 10000, "warm startup index exceeded 10 seconds");

    qInfo().nospace()
        << "RideCache index " << rideCount << ": cold=" << coldMs
        << "ms warm=" << warmMs << "ms";
}

void TestRideCachePerformance::startupBatchesAreBounded()
{
    constexpr qsizetype RideCount = 50000;
    const QVector<RideCacheStartup::BatchRange> batches =
        RideCacheStartup::batchRanges(
            RideCount, RideCacheStartup::BatchSize);

    QCOMPARE(RideCacheStartup::MaximumPendingSnapshotBatches, 4);
    QCOMPARE(batches.size(), 98);
    qsizetype covered = 0;
    for (const RideCacheStartup::BatchRange &batch : batches) {
        QCOMPARE(batch.first, covered);
        QVERIFY(batch.count > 0);
        QVERIFY(batch.count <= RideCacheStartup::BatchSize);
        covered += batch.count;
    }
    QCOMPARE(covered, RideCount);
}

void TestRideCachePerformance::
snapshotApplicationRequiresUntouchedTarget()
{
    const QDateTime dateTime(
        QDate(2026, 7, 18), QTime(8, 30), QTimeZone::UTC);
    const QString fileName =
        QStringLiteral("2026_07_18_08_30_00.json");
    auto target = RideCacheStartup::SnapshotTargetState{
        fileName, dateTime, true, false, false, false, false
    };

    const auto canApply = [&]() {
        return RideCacheStartup::canApplySnapshot(
            fileName, dateTime, target);
    };

    QVERIFY(canApply());

    target.fileName = QStringLiteral("other.json");
    QVERIFY(!canApply());
    target.fileName = fileName;

    target.dateTime = dateTime.addSecs(1);
    QVERIFY(!canApply());
    target.dateTime = dateTime;

    target.stale = false;
    QVERIFY(!canApply());
    target.stale = true;

    target.dirty = true;
    QVERIFY(!canApply());
    target.dirty = false;

    target.editing = true;
    QVERIFY(!canApply());
    target.editing = false;

    target.open = true;
    QVERIFY(!canApply());
    target.open = false;

    target.hasIntervals = true;
    QVERIFY(!canApply());
}

void TestRideCachePerformance::invalidationScope_data()
{
    QTest::addColumn<int>("rideCount");

    QTest::newRow("1k activities") << 1000;
    QTest::newRow("10k activities") << 10000;
    QTest::newRow("50k activities") << 50000;
}

void TestRideCachePerformance::invalidationScope()
{
    QFETCH(int, rideCount);

    const auto cosmetic = RideCacheStartup::planInvalidation(
        CONFIG_NOTECOLOR | CONFIG_FIELDS | CONFIG_APPEARANCE);
    QVERIFY(cosmetic.recolor);
    QVERIFY(cosmetic.rebuildCalendarText);
    QVERIFY(!cosmetic.refreshMetrics);

    int metricCandidates = 0;
    RideCacheStartup::forEachMetricCandidate(
        cosmetic, rideCount, [&](int) { ++metricCandidates; });
    QCOMPARE(metricCandidates, 0);

    const auto metricChange = RideCacheStartup::planInvalidation(
        CONFIG_ZONES | CONFIG_USERMETRICS);
    QVERIFY(metricChange.refreshMetrics);
    RideCacheStartup::forEachMetricCandidate(
        metricChange, rideCount, [&](int) { ++metricCandidates; });
    QCOMPARE(metricCandidates, rideCount);

    const auto unrelated = RideCacheStartup::planInvalidation(
        CONFIG_DEVICES | CONFIG_SEASONS
        | CONFIG_UNITS | CONFIG_WORKOUTS);
    QVERIFY(!unrelated.recolor);
    QVERIFY(!unrelated.rebuildCalendarText);
    QVERIFY(!unrelated.refreshMetrics);
    QVERIFY(!unrelated.invalidateWbal);
}

void TestRideCachePerformance::
refreshGenerationsRejectSupersededWork()
{
    RideCacheStartup::RefreshGeneration generations;

    const quint64 firstRequest = generations.request();
    QCOMPARE(firstRequest, quint64(1));
    const quint64 firstActive = generations.beginLatest();
    QCOMPARE(firstActive, firstRequest);
    QVERIFY(generations.accepts(firstActive));

    const quint64 secondRequest = generations.request();
    const quint64 thirdRequest = generations.request();
    QCOMPARE(secondRequest, quint64(2));
    QCOMPARE(thirdRequest, quint64(3));
    QVERIFY(!generations.accepts(firstActive));
    QVERIFY(generations.finish(firstActive));

    const quint64 latest = generations.beginLatest();
    QCOMPARE(latest, thirdRequest);
    QVERIFY(generations.accepts(latest));
    QVERIFY(!generations.finish(latest));
    QVERIFY(!generations.hasActive());
}

void TestRideCachePerformance::
refreshCancellationSettlesPendingWork()
{
    RideCacheStartup::RefreshGeneration generations;

    const quint64 active = generations.request();
    QCOMPARE(generations.beginLatest(), active);
    generations.request();
    QVERIFY(generations.hasActive());
    QVERIFY(generations.hasPending());

    generations.cancel();
    QVERIFY(!generations.accepts(active));
    QVERIFY(!generations.hasActive());
    QVERIFY(!generations.hasPending());

    const quint64 next = generations.request();
    QCOMPARE(generations.beginLatest(), next);
    QVERIFY(generations.accepts(next));
    QVERIFY(!generations.finish(next));
}

QTEST_GUILESS_MAIN(TestRideCachePerformance)
#include "testRideCachePerformance.moc"
