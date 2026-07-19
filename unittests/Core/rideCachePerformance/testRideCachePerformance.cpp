#include <QtTest>

#include "RideCacheAggregate.h"
#include "RideCacheStartup.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

struct AggregateItem
{
    int bucket = 0;
    QVector<double> values;
    QVector<double> counts;
    QVector<bool> relevant;
};

struct BucketSpecification
{
    int bucket = 0;
};

} // namespace

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
    void batchAggregationPreservesMetricSemantics();
    void batchAggregationScalesWithoutRepeatedMetricReads();
    void batchedMetricRelevancePreservesUnionSemantics();
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

void TestRideCachePerformance::batchAggregationPreservesMetricSemantics()
{
    using MetricType = RideCacheAggregate::MetricType;
    using MetricDefinition = RideCacheAggregate::MetricDefinition;

    const QVector<MetricDefinition> metrics = {
        {MetricType::Total, false, false, 0.0, true},
        {MetricType::RunningTotal, false, false, 0.0, true},
        {MetricType::Average, false, false, 0.0, true},
        {MetricType::Average, true, false, 0.0, true},
        {MetricType::Low, false, false, 0.0, true},
        {MetricType::Peak, false, false, 0.0, true},
        {MetricType::MeanSquareRoot, false, false, 0.0, true},
        {MetricType::Average, false, true, -255.0, true},
        {MetricType::Average, false, false, 0.0, true, false},
        {MetricType::Total, false, false, 0.0, false}
    };
    const QVector<AggregateItem> items = {
        {0,
         {5.0, 5.0, 10.0, 10.0, 5.0, 5.0, 3.0, -255.0, 10.0, 999.0},
         {1.0, 1.0, 2.0, 2.0, 1.0, 1.0, 2.0, 1.0, 2.0, 1.0},
         {false, false, true, false, false, false, false, false, true, true}},
        {0,
         {7.0, 7.0, 0.0, 0.0, -2.0, 8.0, 4.0, 20.0, 0.0, 999.0},
         {1.0, 1.0, 4.0, 4.0, 1.0, 1.0, 2.0, 2.0, 4.0, 1.0},
         {true, false, false, true, false, true, false, true, false, true}},
        {0,
         {std::numeric_limits<double>::quiet_NaN(),
          std::numeric_limits<double>::infinity(),
          30.0, 30.0, 9.0, 2.0, 0.0, 0.0, 30.0, 999.0},
         {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 3.0, 1.0, 1.0},
         {false, true, false, false, true, false, true, false, false, true}}
    };
    const QVector<BucketSpecification> specifications = {{0}};

    int valueCalls = 0;
    int countCalls = 0;
    const auto result = RideCacheAggregate::aggregate(
        items,
        specifications,
        metrics,
        [](const BucketSpecification &specification,
           const AggregateItem &item) {
            return specification.bucket == item.bucket;
        },
        [&](qsizetype metric, const AggregateItem &item) {
            ++valueCalls;
            return item.values.at(metric);
        },
        [&](qsizetype metric, const AggregateItem &item) {
            ++countCalls;
            return item.counts.at(metric);
        });

    QCOMPARE(result.accumulators.size(), 1);
    QCOMPARE(result.accumulators.constFirst().size(), metrics.size());
    const auto value = [&](qsizetype metric) {
        return RideCacheAggregate::finalValue(
            result.accumulators.constFirst().at(metric), metrics.at(metric));
    };
    QCOMPARE(value(0), 12.0);
    QCOMPARE(value(1), 12.0);
    QVERIFY(qAbs(value(2) - (50.0 / 3.0)) < 1e-12);
    QVERIFY(qAbs(value(3) - (50.0 / 7.0)) < 1e-12);
    QCOMPARE(value(4), -2.0);
    QCOMPARE(value(5), 8.0);
    QVERIFY(qAbs(value(6) - std::sqrt(50.0 / 6.0)) < 1e-12);
    QCOMPARE(value(7), 20.0);
    QCOMPARE(value(8), 50.0);
    QCOMPARE(value(9), 0.0);
    QCOMPARE(valueCalls, 27);
    QCOMPARE(countCalls, 27);

}

void TestRideCachePerformance::
batchAggregationScalesWithoutRepeatedMetricReads()
{
    constexpr int RowCount = 50000;
    constexpr int BucketCount = 52;
    constexpr int MetricCount = 6;
    constexpr qint64 BudgetMilliseconds = 2000;

    struct LargeItem { int bucket; };
    QVector<LargeItem> items;
    items.reserve(RowCount);
    for (int row = 0; row < RowCount; ++row) {
        items.append(LargeItem{row % BucketCount});
    }

    QVector<BucketSpecification> specifications;
    specifications.reserve(BucketCount);
    for (int bucket = 0; bucket < BucketCount; ++bucket) {
        specifications.append(BucketSpecification{bucket});
    }

    QVector<RideCacheAggregate::MetricDefinition> metrics;
    metrics.fill(
        {RideCacheAggregate::MetricType::Total,
         false, false, 0.0, true},
        MetricCount);

    qint64 passCalls = 0;
    qint64 valueCalls = 0;
    qint64 countCalls = 0;
    QElapsedTimer timer;
    timer.start();
    const auto result = RideCacheAggregate::aggregate(
        items,
        specifications,
        metrics,
        [&](const BucketSpecification &specification,
            const LargeItem &item) {
            ++passCalls;
            return specification.bucket == item.bucket;
        },
        [&](qsizetype, const LargeItem &) {
            ++valueCalls;
            return 1.0;
        },
        [&](qsizetype, const LargeItem &) {
            ++countCalls;
            return 1.0;
        });
    const qint64 elapsed = timer.elapsed();

    QCOMPARE(passCalls, qint64(RowCount) * BucketCount);
    QCOMPARE(valueCalls, qint64(RowCount) * MetricCount);
    QCOMPARE(countCalls, qint64(RowCount) * MetricCount);
    QCOMPARE(result.accumulators.size(), BucketCount);
    for (int bucket = 0; bucket < BucketCount; ++bucket) {
        const int expectedRows =
            (RowCount + BucketCount - 1 - bucket) / BucketCount;
        for (int metric = 0; metric < MetricCount; ++metric) {
            QCOMPARE(
                RideCacheAggregate::finalValue(
                    result.accumulators.at(bucket).at(metric),
                    metrics.at(metric)),
                double(expectedRows));
        }
    }
    QVERIFY2(
        elapsed < BudgetMilliseconds,
        qPrintable(
            QStringLiteral("50k x 52 x 6 batch aggregation took %1 ms")
                .arg(elapsed)));
}

void TestRideCachePerformance::
batchedMetricRelevancePreservesUnionSemantics()
{
    const QVector<AggregateItem> items = {
        {0, {}, {}, {true, false, false, true}},
        {1, {}, {}, {false, true, false, false}},
        {2, {}, {}, {false, false, true, true}}
    };
    const QVector<BucketSpecification> specifications = {{0}, {2}};

    int passCalls = 0;
    int relevanceCalls = 0;
    const QVector<bool> relevant = RideCacheAggregate::metricRelevance(
        items,
        specifications,
        4,
        [&](const BucketSpecification &specification,
            const AggregateItem &item) {
            ++passCalls;
            return specification.bucket == item.bucket;
        },
        [&](qsizetype metric, const AggregateItem &item) {
            ++relevanceCalls;
            return item.relevant.at(metric);
        });

    QCOMPARE(relevant, QVector<bool>({true, false, true, true}));
    QCOMPARE(passCalls, 5);
    QCOMPARE(relevanceCalls, 6);
}

QTEST_GUILESS_MAIN(TestRideCachePerformance)
#include "testRideCachePerformance.moc"
