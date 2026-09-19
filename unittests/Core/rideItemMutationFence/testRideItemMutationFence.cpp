#include <QtTest>

#include "IntervalItem.h"
#include "RideItem.h"
#include "RideRefreshTargetRegistry.h"

#include <functional>
#include <memory>

class RideItemRefreshTestAccess
{
public:
    using Registry = RideRefreshTargetRegistry<RideItem>;

    static bool advance(void *context, RideItem *item)
    {
        Registry *registry = static_cast<Registry *>(context);
        const Registry::InvalidationResult result =
            registry->invalidateTarget(item);
        return result == Registry::InvalidationResult::Advanced
            || result == Registry::InvalidationResult::Retired;
    }

    static void attach(RideItem &item, Registry &registry)
    {
        item.refreshMutationContextForTest_ = &registry;
        item.refreshMutationAdvanceForTest_ = &advance;
        item.refreshTargetRegistered_.store(
            true, std::memory_order_release);
    }

    static void bindOpenMarker(RideItem &item, QObject *marker)
    {
        item.ride_ = reinterpret_cast<RideFile *>(marker);
    }
};

class TestRideItemMutationFence : public QObject
{
    Q_OBJECT

private slots:
    void productionMutatorsInvalidateCapturedTokens();
};

void TestRideItemMutationFence::
productionMutatorsInvalidateCapturedTokens()
{
    using Registry = RideItemRefreshTestAccess::Registry;
    Registry registry;
    QVERIFY(registry.initialize(QThread::currentThread(), 91));

    auto item = std::make_unique<RideItem>(RideItem::MutationTestTag {});
    RideItemRefreshTestAccess::attach(*item, registry);

    const auto expectInvalidated =
        [&](const std::function<void()> &mutation) {
            const RideRefreshTargetToken before =
                registry.registerTarget(item.get());
            QVERIFY(before.isValid());
            mutation();
            QVERIFY(!registry.resolve(before));
            const RideRefreshTargetToken after =
                registry.registerTarget(item.get());
            QVERIFY(after.isValid());
            QCOMPARE(after.targetId, before.targetId);
            QVERIFY(after.revision > before.revision);
            QCOMPARE(registry.resolve(after), item.get());
        };

    expectInvalidated([&]() {
        item->setFileName(
            QStringLiteral("/activities"),
            QStringLiteral("activity.fit"));
    });
    expectInvalidated([&]() { item->clearIntervals(); });

    IntervalItem interval(
        IntervalItem::MutationTestTag {}, item.get());
    expectInvalidated([&]() { QVERIFY(interval.setSelected(true)); });
    expectInvalidated([&]() { interval.setDisplaySequence(7); });
    QCOMPARE(interval.displaySequence, 7);

    QObject openMarker;
    RideItemRefreshTestAccess::bindOpenMarker(*item, &openMarker);
    expectInvalidated([&]() { item->rideFileDestroyed(&openMarker); });

    QVERIFY(registry.retire(item.get()));
}

QTEST_GUILESS_MAIN(TestRideItemMutationFence)

#include "testRideItemMutationFence.moc"
