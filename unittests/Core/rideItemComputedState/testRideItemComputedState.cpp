/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

#include "IntervalItem.h"
#include "RideCacheSnapshot.h"
#include "RideItem.h"
#include "RideItemRefreshResult.h"

class RideItemRefreshTestAccess
{
public:
    static void populate(RideItem &item)
    {
        item.metrics_ = {1.0, 2.0};
        item.count_ = {3.0, 4.0};
        item.stdmean_.insert(1, 5.0);
        item.stdvariance_.insert(1, 6.0);
        item.metadata_.insert(QStringLiteral("Mood"), QStringLiteral("Good"));
        item.xdata_.insert(
            QStringLiteral("EXTRA"), {QStringLiteral("value")});
        item.errors_ = {QStringLiteral("warning")};
        item.zoneRange = 1;
        item.hrZoneRange = 2;
        item.paceZoneRange = 3;
        item.fingerprint = 4;
        item.metacrc = 5;
        item.crc = 6;
        item.timestamp = 7;
        item.dbversion = 8;
        item.udbversion = 9;
        item.color = Qt::red;
        item.present = QStringLiteral("P");
        item.sport = QStringLiteral("Bike");
        item.isBike = true;
        item.isRun = false;
        item.isSwim = false;
        item.isXtrain = false;
        item.isAero = true;
        item.weight = 72.5;
        item.overrides_ = {QStringLiteral("workout_time")};
        item.samples = true;

        auto interval = new IntervalItem;
        interval->rideItem_ = &item;
        interval->name = QStringLiteral("Lap");
        interval->type = RideFileInterval::DEVICE;
        interval->start = 10.0;
        interval->stop = 20.0;
        interval->startKM = 1.0;
        interval->stopKM = 2.0;
        interval->displaySequence = 11;
        interval->color = Qt::blue;
        interval->route = QUuid::createUuid();
        interval->test = true;
        interval->selected = true;
        interval->metrics_ = {12.0};
        interval->count_ = {13.0};
        interval->stdmean_.insert(0, 14.0);
        interval->stdvariance_.insert(0, 15.0);
        item.intervals_.append(interval);
    }

    static RideItemComputedState take(RideItem &item)
    {
        return RideItemComputedState::takeFrom(item);
    }

    static void apply(RideItemComputedState &state, RideItem &item)
    {
        state.applyTo(item);
    }

    static void publish(
        RideItemComputedState &state,
        RideItem &item,
        const std::function<void()> &notify)
    {
        state.publishTo(item, notify);
    }

    static void failMaterializationAfter(int intervals)
    {
        RideItemComputedState::failMaterializationAfterForTest(intervals);
    }

    static void appendInterval(RideItem &item, IntervalItem *interval)
    {
        item.intervals_.append(interval);
    }

    static qsizetype intervalCount(const RideItem &item)
    {
        return item.intervals_.size();
    }

    static IntervalItem *firstInterval(const RideItem &item)
    {
        return item.intervals_.constFirst();
    }

    static bool metricsEmpty(const RideItem &item)
    {
        return item.metrics_.isEmpty();
    }

    static void bindRide(RideItem &item, RideFile *ride)
    {
        item.ride_ = ride;
        item.ownsRide_ = false;
    }

    static void setStartupIdentity(
        RideItem &item,
        const QString &fileName,
        const QDateTime &dateTime)
    {
        item.fileName = fileName;
        item.dateTime = dateTime;
    }

    static void setStartupSourceFlags(RideItem &item)
    {
        item.isdirty = true;
        item.isstale = false;
        item.isedit = true;
        item.skipsave = true;
    }

    static void verifyStartupFlags(const RideItem &item)
    {
        QVERIFY(item.isdirty);
        QVERIFY(!item.isstale);
        QVERIFY(item.isedit);
        QVERIFY(item.skipsave);
    }

    static bool sourceIntervalsEmpty(const RideItem &item)
    {
        return item.intervals_.isEmpty();
    }

    static void verifyComputed(const RideItem &item)
    {
        QCOMPARE(item.metrics_, QVector<double>({1.0, 2.0}));
        QCOMPARE(item.count_, QVector<double>({3.0, 4.0}));
        QCOMPARE(item.stdmean_.value(1), 5.0);
        QCOMPARE(item.stdvariance_.value(1), 6.0);
        QCOMPARE(item.metadata_.value(QStringLiteral("Mood")),
                 QStringLiteral("Good"));
        QCOMPARE(item.xdata_.value(QStringLiteral("EXTRA")),
                 QStringList({QStringLiteral("value")}));
        QCOMPARE(item.errors_, QStringList({QStringLiteral("warning")}));
        QCOMPARE(item.zoneRange, 1);
        QCOMPARE(item.hrZoneRange, 2);
        QCOMPARE(item.paceZoneRange, 3);
        QCOMPARE(item.fingerprint, 4UL);
        QCOMPARE(item.metacrc, 5UL);
        QCOMPARE(item.crc, 6UL);
        QCOMPARE(item.timestamp, 7UL);
        QCOMPARE(item.dbversion, 8);
        QCOMPARE(item.udbversion, 9);
        QCOMPARE(item.color, QColor(Qt::red));
        QCOMPARE(item.present, QStringLiteral("P"));
        QCOMPARE(item.sport, QStringLiteral("Bike"));
        QVERIFY(item.isBike);
        QVERIFY(!item.isRun);
        QVERIFY(!item.isSwim);
        QVERIFY(!item.isXtrain);
        QVERIFY(item.isAero);
        QCOMPARE(item.weight, 72.5);
        QCOMPARE(item.overrides_, QStringList({QStringLiteral("workout_time")}));
        QVERIFY(item.samples);
        QCOMPARE(item.intervals_.size(), 1);
        const IntervalItem *interval = item.intervals_.constFirst();
        QCOMPARE(interval->rideItem_, &item);
        QCOMPARE(interval->rideInterval, nullptr);
        QCOMPARE(interval->name, QStringLiteral("Lap"));
        QCOMPARE(interval->type, RideFileInterval::DEVICE);
        QCOMPARE(interval->start, 10.0);
        QCOMPARE(interval->stop, 20.0);
        QCOMPARE(interval->startKM, 1.0);
        QCOMPARE(interval->stopKM, 2.0);
        QCOMPARE(interval->displaySequence, 11);
        QCOMPARE(interval->color, QColor(Qt::blue));
        QVERIFY(!interval->route.isNull());
        QVERIFY(interval->test);
        QVERIFY(interval->selected);
        QCOMPARE(interval->metrics_, QVector<double>({12.0}));
        QCOMPARE(interval->count_, QVector<double>({13.0}));
        QCOMPARE(interval->stdmean_.value(0), 14.0);
        QCOMPARE(interval->stdvariance_.value(0), 15.0);
    }

    static void setUiFlags(RideItem &item)
    {
        item.isdirty = true;
        item.isedit = true;
        item.skipsave = true;
    }

    static void verifyUiFlags(const RideItem &item)
    {
        QVERIFY(item.isdirty);
        QVERIFY(item.isedit);
        QVERIFY(item.skipsave);
    }
};

RideItem::RideItem()
    : ride_(nullptr)
    , fileCache_(nullptr)
    , context(nullptr)
    , isdirty(false)
    , isstale(true)
    , isedit(false)
    , skipsave(false)
    , color(QColor(1, 1, 1))
    , isBike(false)
    , isRun(false)
    , isSwim(false)
    , isXtrain(false)
    , isAero(false)
    , samples(false)
    , zoneRange(-1)
    , hrZoneRange(-1)
    , paceZoneRange(-1)
    , fingerprint(0)
    , metacrc(0)
    , crc(0)
    , timestamp(0)
    , dbversion(0)
    , udbversion(0)
    , weight(0)
{
}

RideItem::~RideItem()
{
    qDeleteAll(intervals_);
}

bool RideItem::isOpen() { return ride_ != nullptr; }
bool RideItem::prepareForRefreshRelevantMutation() { return true; }
void RideItem::modified() {}
void RideItem::saved() {}
void RideItem::reverted() {}
void RideItem::notifyRideDataChanged() {}
void RideItem::notifyRideMetadataChanged() {}
void RideItem::rideFileDestroyed(QObject *) { ride_ = nullptr; }

RideFile::RideFile()
    : context(nullptr)
    , wstale(true)
    , recIntSecs_(0.0)
    , minPoint(new RideFilePoint)
    , maxPoint(new RideFilePoint)
    , avgPoint(new RideFilePoint)
    , totalPoint(new RideFilePoint)
    , data(nullptr)
    , wprime_(nullptr)
    , weight_(0.0)
    , totalCount(0.0)
    , totalTemp(0.0)
    , dstale(true)
    , windSpeed_(0.0)
    , windHeading_(0.0)
{
    command = nullptr;
}

RideFile::~RideFile()
{
    qDeleteAll(dataPoints_);
    qDeleteAll(referencePoints_);
    qDeleteAll(intervals_);
    qDeleteAll(calibrations_);
    qDeleteAll(xdata_);
    delete minPoint;
    delete maxPoint;
    delete avgPoint;
    delete totalPoint;
}

IntervalItem::IntervalItem()
    : rideItem_(nullptr)
    , selected(false)
    , type(RideFileInterval::USER)
    , start(0)
    , stop(0)
    , startKM(0)
    , stopKM(0)
    , displaySequence(0)
    , color(Qt::black)
    , test(false)
    , rideInterval(nullptr)
{
}

class TestRideItemComputedState : public QObject
{
    Q_OBJECT

private slots:
    void detachedStatePreservesFieldsAndCanonicalUiFlags();
    void publicationSwapsBeforeOneNotificationAndRetiresAfterward();
    void materializationFailureLeavesCanonicalStateUntouched();
    void userIntervalRebindsByOrdinal();
    void emptyStatePublishesOnce();
    void startupSnapshotPreservesFlagsAndComputedState();
    void rejectedStartupSnapshotLeavesCanonicalStateUntouched();
    void refreshOutcomeClassifiesOnlyPublishedStateAsSuccess();
};

void TestRideItemComputedState::
detachedStatePreservesFieldsAndCanonicalUiFlags()
{
    RideItem source;
    RideItemRefreshTestAccess::populate(source);
    RideItemComputedState state = RideItemRefreshTestAccess::take(source);
    QVERIFY(RideItemRefreshTestAccess::sourceIntervalsEmpty(source));

    RideItem target;
    RideItemRefreshTestAccess::setUiFlags(target);
    RideItemRefreshTestAccess::apply(state, target);

    RideItemRefreshTestAccess::verifyComputed(target);
    RideItemRefreshTestAccess::verifyUiFlags(target);
}

void TestRideItemComputedState::
publicationSwapsBeforeOneNotificationAndRetiresAfterward()
{
    RideItem source;
    RideItemRefreshTestAccess::populate(source);
    RideItemComputedState state = RideItemRefreshTestAccess::take(source);

    RideItem target;
    auto retired = new IntervalItem;
    retired->rideItem_ = &target;
    retired->name = QStringLiteral("Retired");
    RideItemRefreshTestAccess::appendInterval(target, retired);

    int notifications = 0;
    RideItemRefreshTestAccess::publish(
        state, target, [&]() {
            ++notifications;
            QCOMPARE(RideItemRefreshTestAccess::intervalCount(target), 1);
            QVERIFY(
                RideItemRefreshTestAccess::firstInterval(target) != retired);
            QCOMPARE(retired->name, QStringLiteral("Retired"));
        });

    QCOMPARE(notifications, 1);
    RideItemRefreshTestAccess::verifyComputed(target);
}

void TestRideItemComputedState::
materializationFailureLeavesCanonicalStateUntouched()
{
    RideItem source;
    RideItemRefreshTestAccess::populate(source);
    RideItemComputedState state = RideItemRefreshTestAccess::take(source);

    RideItem target;
    auto existing = new IntervalItem;
    existing->rideItem_ = &target;
    existing->name = QStringLiteral("Existing");
    RideItemRefreshTestAccess::appendInterval(target, existing);

    int notifications = 0;
    RideItemRefreshTestAccess::failMaterializationAfter(0);
    QVERIFY_THROWS_EXCEPTION(
        std::bad_alloc,
        RideItemRefreshTestAccess::publish(
            state, target, [&]() { ++notifications; }));
    RideItemRefreshTestAccess::failMaterializationAfter(-1);

    QCOMPARE(notifications, 0);
    QCOMPARE(RideItemRefreshTestAccess::intervalCount(target), 1);
    QCOMPARE(RideItemRefreshTestAccess::firstInterval(target), existing);
    QCOMPARE(existing->name, QStringLiteral("Existing"));
    QVERIFY(RideItemRefreshTestAccess::metricsEmpty(target));
}

void TestRideItemComputedState::userIntervalRebindsByOrdinal()
{
    RideFile ride;
    ride.addInterval(
        RideFileInterval::DEVICE, 0.0, 1.0,
        QStringLiteral("Device"));
    ride.addInterval(
        RideFileInterval::USER, 1.0, 2.0,
        QStringLiteral("User"));

    RideItem source;
    RideItemRefreshTestAccess::bindRide(source, &ride);
    auto interval = new IntervalItem;
    interval->rideItem_ = &source;
    interval->type = RideFileInterval::USER;
    interval->rideInterval = ride.intervals().at(1);
    RideItemRefreshTestAccess::appendInterval(source, interval);
    RideItemComputedState state = RideItemRefreshTestAccess::take(source);

    RideItem target;
    RideItemRefreshTestAccess::bindRide(target, &ride);
    RideItemRefreshTestAccess::apply(state, target);
    QCOMPARE(
        RideItemRefreshTestAccess::firstInterval(target)->rideInterval,
        ride.intervals().at(1));
}

void TestRideItemComputedState::emptyStatePublishesOnce()
{
    RideItem source;
    RideItemComputedState state = RideItemRefreshTestAccess::take(source);
    RideItem target;
    auto retired = new IntervalItem;
    retired->rideItem_ = &target;
    RideItemRefreshTestAccess::appendInterval(target, retired);

    int notifications = 0;
    RideItemRefreshTestAccess::publish(
        state, target, [&]() { ++notifications; });
    QCOMPARE(notifications, 1);
    QCOMPARE(RideItemRefreshTestAccess::intervalCount(target), 0);
}

void TestRideItemComputedState::
startupSnapshotPreservesFlagsAndComputedState()
{
    const QString fileName = QStringLiteral("activity.fit");
    const QDateTime dateTime(
        QDate(2026, 9, 19), QTime(12, 0), QTimeZone::UTC);
    RideItem source;
    RideItemRefreshTestAccess::populate(source);
    RideItemRefreshTestAccess::setStartupIdentity(
        source, fileName, dateTime);
    RideItemRefreshTestAccess::setStartupSourceFlags(source);
    RideCacheItemSnapshot snapshot =
        RideCacheItemSnapshot::takeFrom(source);

    RideItem target;
    RideItemRefreshTestAccess::setStartupIdentity(
        target, fileName, dateTime);
    QVERIFY(snapshot.applyTo(target));
    RideItemRefreshTestAccess::verifyComputed(target);
    RideItemRefreshTestAccess::verifyStartupFlags(target);
}

void TestRideItemComputedState::
rejectedStartupSnapshotLeavesCanonicalStateUntouched()
{
    const QString fileName = QStringLiteral("activity.fit");
    const QDateTime dateTime(
        QDate(2026, 9, 19), QTime(12, 0), QTimeZone::UTC);
    RideItem source;
    RideItemRefreshTestAccess::populate(source);
    RideItemRefreshTestAccess::setStartupIdentity(
        source, fileName, dateTime);
    RideCacheItemSnapshot snapshot =
        RideCacheItemSnapshot::takeFrom(source);

    RideItem target;
    RideItemRefreshTestAccess::setStartupIdentity(
        target, fileName, dateTime);
    RideItemRefreshTestAccess::setUiFlags(target);
    auto existing = new IntervalItem;
    existing->rideItem_ = &target;
    existing->name = QStringLiteral("Existing");
    RideItemRefreshTestAccess::appendInterval(target, existing);

    QVERIFY(!snapshot.applyTo(target));
    QCOMPARE(RideItemRefreshTestAccess::intervalCount(target), 1);
    QCOMPARE(RideItemRefreshTestAccess::firstInterval(target), existing);
    QCOMPARE(existing->name, QStringLiteral("Existing"));
    QVERIFY(RideItemRefreshTestAccess::metricsEmpty(target));
    RideItemRefreshTestAccess::verifyUiFlags(target);
}

void TestRideItemComputedState::
refreshOutcomeClassifiesOnlyPublishedStateAsSuccess()
{
    QVERIFY(rideItemRefreshSucceeded(
        RideItemRefreshOutcome::AlreadyCurrent));
    QVERIFY(rideItemRefreshSucceeded(
        RideItemRefreshOutcome::Published));
    QVERIFY(!rideItemRefreshSucceeded(
        RideItemRefreshOutcome::ReadyForPublication));
    QVERIFY(!rideItemRefreshSucceeded(
        RideItemRefreshOutcome::SourceFingerprintFailed));
    QVERIFY(!rideItemRefreshSucceeded(
        RideItemRefreshOutcome::SourceOpenFailed));
    QVERIFY(!rideItemRefreshSucceeded(
        RideItemRefreshOutcome::EnvironmentFingerprintUnavailable));
    QVERIFY(!rideItemRefreshSucceeded(
        RideItemRefreshOutcome::EnvironmentMetricRegistryUnavailable));
    QVERIFY(!rideItemRefreshSucceeded(
        RideItemRefreshOutcome::CachePreparationInvalid));
    QVERIFY(!rideItemRefreshSucceeded(
        RideItemRefreshOutcome::IdentityRejected));
    QVERIFY(!rideItemRefreshSucceeded(
        RideItemRefreshOutcome::CacheSourceRejected));
}

QTEST_GUILESS_MAIN(TestRideItemComputedState)

#include "testRideItemComputedState.moc"
