#include <QtTest>

#include "RideCacheBulkMerge.h"
#include "RideImportRideStore.h"

#include <algorithm>

namespace {

struct TrackedRide
{
    explicit TrackedRide(int identifier) : id(identifier) {}
    ~TrackedRide() { ++destroyed; }

    int id;
    static int destroyed;
};

int TrackedRide::destroyed = 0;

struct MergeItem
{
    QString fileName;
    QDateTime dateTime;
    int id;
};

MergeItem *item(
    const QString &fileName, const QDateTime &dateTime, int id)
{
    return new MergeItem{fileName, dateTime, id};
}

void deleteItems(const QVector<MergeItem *> &items)
{
    for (MergeItem *entry : items) delete entry;
}

} // namespace

class TestRideImportBatch : public QObject
{
    Q_OBJECT

private slots:
    void parseOnceReusesValidatedRide();
    void failedParseIsNotRetried();
    void parseOnceBatch_data();
    void parseOnceBatch();
    void rowMutationsPreserveOwnershipAndErrors();
    void bulkMerge_data();
    void bulkMerge();
    void duplicateReplacementIsExplicit();
    void emptyMergeDoesNotResetModel();
};

void TestRideImportBatch::parseOnceReusesValidatedRide()
{
    TrackedRide::destroyed = 0;
    int parserCalls = 0;

    {
        BasicRideImportRideStore<TrackedRide> store;
        store.appendPending();

        auto parser = [&](QStringList &errors) {
            ++parserCalls;
            errors.append(QStringLiteral("validation warning"));
            return new TrackedRide(7);
        };

        TrackedRide *first = store.parseOnce(0, parser);
        TrackedRide *second = store.parseOnce(
            0, [&](QStringList &) {
                ++parserCalls;
                return new TrackedRide(99);
            });

        QCOMPARE(first, second);
        QCOMPARE(first->id, 7);
        QCOMPARE(parserCalls, 1);
        QCOMPARE(
            store.errorsAt(0),
            QStringList{QStringLiteral("validation warning")});
        QVERIFY(store.wasParsed(0));
    }

    QCOMPARE(TrackedRide::destroyed, 1);
}

void TestRideImportBatch::failedParseIsNotRetried()
{
    TrackedRide::destroyed = 0;
    int parserCalls = 0;

    BasicRideImportRideStore<TrackedRide> store;
    store.appendPending();

    TrackedRide *first = store.parseOnce(
        0, [&](QStringList &errors) -> TrackedRide * {
            ++parserCalls;
            errors.append(QStringLiteral("invalid activity"));
            return nullptr;
        });
    TrackedRide *second = store.parseOnce(
        0, [&](QStringList &) {
            ++parserCalls;
            return new TrackedRide(99);
        });

    QCOMPARE(first, nullptr);
    QCOMPARE(second, nullptr);
    QCOMPARE(parserCalls, 1);
    QCOMPARE(
        store.errorsAt(0),
        QStringList{QStringLiteral("invalid activity")});
    QVERIFY(store.wasParsed(0));
    QCOMPARE(TrackedRide::destroyed, 0);
}

void TestRideImportBatch::parseOnceBatch_data()
{
    QTest::addColumn<int>("rideCount");

    QTest::newRow("100 imports") << 100;
    QTest::newRow("1000 imports") << 1000;
}

void TestRideImportBatch::parseOnceBatch()
{
    QFETCH(int, rideCount);

    TrackedRide::destroyed = 0;
    int parserCalls = 0;

    {
        BasicRideImportRideStore<TrackedRide> store;
        for (int i = 0; i < rideCount; ++i) store.appendPending();

        for (int i = 0; i < rideCount; ++i) {
            TrackedRide *first = store.parseOnce(
                i, [&](QStringList &) {
                    ++parserCalls;
                    return new TrackedRide(i);
                });
            TrackedRide *second = store.parseOnce(
                i, [&](QStringList &) {
                    ++parserCalls;
                    return new TrackedRide(-1);
                });

            QCOMPARE(first, second);
            QCOMPARE(first->id, i);
        }

        QCOMPARE(parserCalls, rideCount);
    }

    QCOMPARE(TrackedRide::destroyed, rideCount);
}

void TestRideImportBatch::rowMutationsPreserveOwnershipAndErrors()
{
    TrackedRide::destroyed = 0;

    {
        BasicRideImportRideStore<TrackedRide> store;
        store.appendPending();
        store.insertParsed(
            1, new TrackedRide(2),
            QStringList{QStringLiteral("second")});
        store.insertParsed(
            1, new TrackedRide(1),
            QStringList{QStringLiteral("first")});

        QCOMPARE(store.size(), 3);
        QCOMPARE(store.at(1)->id, 1);
        QCOMPARE(store.at(2)->id, 2);
        QCOMPARE(
            store.errorsAt(2),
            QStringList{QStringLiteral("second")});

        TrackedRide *taken = store.take(1);
        QVERIFY(taken);
        QCOMPARE(taken->id, 1);
        QCOMPARE(store.at(1), nullptr);
        delete taken;
        QCOMPARE(TrackedRide::destroyed, 1);

        store.removeAt(2);
        QCOMPARE(TrackedRide::destroyed, 2);
        QCOMPARE(store.size(), 2);

        QCOMPARE(store.takeAndRemove(0), nullptr);
        QCOMPARE(store.size(), 1);
        store.insertParsed(1, new TrackedRide(3));
    }

    QCOMPARE(TrackedRide::destroyed, 3);
}

void TestRideImportBatch::bulkMerge_data()
{
    QTest::addColumn<int>("additionCount");

    QTest::newRow("100 imports") << 100;
    QTest::newRow("1000 imports") << 1000;
}

void TestRideImportBatch::bulkMerge()
{
    QFETCH(int, additionCount);

    const QDateTime base(
        QDate(2020, 1, 1), QTime(0, 0), QTimeZone::UTC);
    QVector<MergeItem *> current;
    QVector<MergeItem *> additions;
    current.reserve(10000 + additionCount);
    additions.reserve(additionCount);

    for (int i = 0; i < 10000; ++i) {
        current.append(item(
            QStringLiteral("existing-%1").arg(i),
            base.addSecs(i), i));
    }
    for (int i = additionCount - 1; i >= 0; --i) {
        additions.append(item(
            QStringLiteral("incoming-%1").arg(i),
            base.addSecs(10000 + i), 10000 + i));
    }

    int beginResetCalls = 0;
    int endResetCalls = 0;
    qsizetype comparisons = 0;
    const QVector<MergeItem *> replaced =
        RideCacheBulkMerge::mergeItems(
            current,
            additions,
            [](const MergeItem *entry) { return entry->fileName; },
            [&](const MergeItem *left, const MergeItem *right) {
                ++comparisons;
                return left->dateTime < right->dateTime;
            },
            [&]() { ++beginResetCalls; },
            [&]() { ++endResetCalls; });

    QVERIFY(replaced.isEmpty());
    QCOMPARE(current.size(), 10000 + additionCount);
    QCOMPARE(beginResetCalls, 1);
    QCOMPARE(endResetCalls, 1);
    QVERIFY(comparisons < 1000000);
    QVERIFY(std::is_sorted(
        current.cbegin(), current.cend(),
        [](const MergeItem *left, const MergeItem *right) {
            return left->dateTime < right->dateTime;
        }));

    deleteItems(current);
}

void TestRideImportBatch::duplicateReplacementIsExplicit()
{
    const QDateTime base(
        QDate(2020, 1, 1), QTime(0, 0), QTimeZone::UTC);
    MergeItem *old = item(QStringLiteral("same"), base, 1);
    MergeItem *replacement =
        item(QStringLiteral("same"), base.addSecs(2), 2);
    MergeItem *added =
        item(QStringLiteral("new"), base.addSecs(1), 3);
    QVector<MergeItem *> current{old};
    const QVector<MergeItem *> additions{replacement, added};
    int beginResetCalls = 0;
    int endResetCalls = 0;

    const QVector<MergeItem *> replaced =
        RideCacheBulkMerge::mergeItems(
            current,
            additions,
            [](const MergeItem *entry) { return entry->fileName; },
            [](const MergeItem *left, const MergeItem *right) {
                return left->dateTime < right->dateTime;
            },
            [&]() { ++beginResetCalls; },
            [&]() { ++endResetCalls; });

    QCOMPARE(replaced, QVector<MergeItem *>{old});
    QCOMPARE(current, QVector<MergeItem *>({added, replacement}));
    QCOMPARE(beginResetCalls, 1);
    QCOMPARE(endResetCalls, 1);

    deleteItems(replaced);
    deleteItems(current);
}

void TestRideImportBatch::emptyMergeDoesNotResetModel()
{
    QVector<MergeItem *> current;
    const QVector<MergeItem *> additions;
    int beginResetCalls = 0;
    int endResetCalls = 0;

    const QVector<MergeItem *> replaced =
        RideCacheBulkMerge::mergeItems(
            current,
            additions,
            [](const MergeItem *entry) { return entry->fileName; },
            [](const MergeItem *left, const MergeItem *right) {
                return left->dateTime < right->dateTime;
            },
            [&]() { ++beginResetCalls; },
            [&]() { ++endResetCalls; });

    QVERIFY(replaced.isEmpty());
    QVERIFY(current.isEmpty());
    QCOMPARE(beginResetCalls, 0);
    QCOMPARE(endResetCalls, 0);
}

QTEST_GUILESS_MAIN(TestRideImportBatch)
#include "testRideImportBatch.moc"
