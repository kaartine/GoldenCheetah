/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <QtTest>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#define private public
#include "DataFilter.h"
#include "RideMetric.h"
#undef private

#include "IntervalItem.h"
#include "RideItem.h"
#include "Specification.h"

#include "UserMetricRegistrySafetyTestTypes.h"

namespace {

UserMetricSettings settings(const QString &symbol, quint64 version)
{
    UserMetricSettings result;
    result.symbol = symbol;
    result.name = symbol;
    result.description = QStringLiteral("registry lifetime test");
    result.unitsMetric = QStringLiteral("u");
    result.unitsImperial = QStringLiteral("u");
    result.type = RideMetric::Average;
    result.precision = 0;
    result.aggzero = false;
    result.istime = false;
    result.conversion = 1.0;
    result.conversionSum = 0.0;
    result.program = QStringLiteral("value { return %1; }").arg(version);
    result.fingerprint = QString::number(version);
    return result;
}

class ReloadMetric final : public RideMetric
{
public:
    ReloadMetric(QString symbol, quint64 version, bool user,
                 std::atomic<int> *liveDefinitions = nullptr)
        : version_(version), user_(user), liveDefinitions_(liveDefinitions)
    {
        setSymbol(std::move(symbol));
    }

    ReloadMetric(const ReloadMetric &other)
        : RideMetric(other), version_(other.version_), user_(other.user_),
          liveDefinitions_(other.liveDefinitions_), registered_(true)
    {
        if (liveDefinitions_) liveDefinitions_->fetch_add(1);
    }

    ~ReloadMetric() override
    {
        if (registered_ && liveDefinitions_)
            liveDefinitions_->fetch_sub(1);
    }

    bool isUser() const override { return user_; }
    RideMetric *clone() const override { return new ReloadMetric(*this); }

    void compute(RideItem *, Specification,
                 const QHash<QString, RideMetric *> &dependencies) override
    {
        double value = double(version_);
        for (RideMetric *dependency : dependencies) {
            if (dependency) value += dependency->value();
        }
        setValue(value);
    }

private:
    quint64 version_;
    bool user_;
    std::atomic<int> *liveDefinitions_;
    bool registered_ = false;
};

const QString builtinSymbol = QStringLiteral("metric002_registry_builtin");
const QString userSymbol = QStringLiteral("metric002_registry_user");
const QString secondUserSymbol = QStringLiteral("metric002_registry_user_2");
const QString dependencySymbol =
    QStringLiteral("metric002_registry_dependency");
const QString dependencyRootSymbol =
    QStringLiteral("metric002_registry_dependency_root");
const QString blockingSymbol = QStringLiteral("metric002_registry_blocking");

struct BlockingState
{
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
};

class BlockingMetric final : public RideMetric
{
public:
    explicit BlockingMetric(std::shared_ptr<BlockingState> state)
        : state_(std::move(state))
    {
        setSymbol(blockingSymbol);
    }

    BlockingMetric(const BlockingMetric &other)
        : RideMetric(other), state_(other.state_)
    {
    }

    RideMetric *clone() const override { return new BlockingMetric(*this); }

    void compute(RideItem *, Specification,
                 const QHash<QString, RideMetric *> &) override
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->entered = true;
        state_->condition.notify_all();
        state_->condition.wait(lock, [this] { return state_->released; });
        setValue(1.0);
    }

private:
    std::shared_ptr<BlockingState> state_;
};

} // namespace

class TestUserMetricRegistrySafety : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void firstAthleteContextIsNotRetained();
    void concurrentEvaluationAndReloadAreSafe();
    void repeatedReloadPreservesOrderingSchemaAndDependencies();
    void activeEvaluationDoesNotBlockPublication();
    void clonesReuseContextFreeCompiledProgramAfterDefinitionTeardown();
    void removedUserMetricDefinitionsAreReclaimed();

private:
    int builtinCount_ = 0;
    std::shared_ptr<BlockingState> blockingState_;
};

void TestUserMetricRegistrySafety::initTestCase()
{
    ReloadMetric builtin(builtinSymbol, 1, false);
    QVERIFY(RideMetricFactory::instance().addMetric(builtin));
    ReloadMetric dependency(dependencySymbol, 2, false);
    QVERIFY(RideMetricFactory::instance().addMetric(dependency));
    const QVector<QString> dependencies{dependencySymbol};
    ReloadMetric dependencyRoot(dependencyRootSymbol, 3, false);
    QVERIFY(RideMetricFactory::instance().addMetric(
        dependencyRoot, &dependencies));
    blockingState_ = std::make_shared<BlockingState>();
    BlockingMetric blocking(blockingState_);
    QVERIFY(RideMetricFactory::instance().addMetric(blocking));
    builtinCount_ = RideMetricFactory::instance().metricCount();
}

void TestUserMetricRegistrySafety::cleanup()
{
    RideMetricFactory::instance().replaceUserMetrics({}, 0);
}

void TestUserMetricRegistrySafety::firstAthleteContextIsNotRetained()
{
    auto first = std::make_unique<TestAthleteContext>();
    first->value = 11;
    Context *firstAddress = asContext(first.get());
    UserMetric original(firstAddress, settings(userSymbol, 1));
    const bool retainedFirstContext = original.program->context == firstAddress;

    first.reset();

    auto second = std::make_unique<TestAthleteContext>();
    second->value = 22;
    RideItem item;
    item.context = asContext(second.get());
    IntervalItem interval;
    std::unique_ptr<RideMetric> evaluation(original.clone());
    evaluation->compute(&item, Specification(&interval, 1.0), {});
    QCOMPARE(evaluation->value(), 22.0);

    UserMetric reloaded(asContext(second.get()), settings(userSymbol, 2));
    std::unique_ptr<RideMetric> reloadedEvaluation(reloaded.clone());
    reloadedEvaluation->compute(
        &item, Specification(&interval, 1.0), {});
    QCOMPARE(reloadedEvaluation->value(), 22.0);

    const QList<UserMetricSettings> reloadedSettings{
        settings(userSymbol, 3)};
    const quint16 schema =
        RideMetric::userMetricFingerprint(reloadedSettings);
    QVERIFY(RideMetricFactory::instance().replaceUserMetrics(
        reloadedSettings, schema));
    const auto published = RideMetric::computeMetrics(
        &item, Specification(&interval, 1.0), {userSymbol});
    QCOMPARE(published.value(userSymbol)->value(), 22.0);
    QVERIFY2(!retainedFirstContext,
             "the globally compiled user metric retained the first athlete");
}

void TestUserMetricRegistrySafety::concurrentEvaluationAndReloadAreSafe()
{
    QList<UserMetricSettings> initial{settings(userSymbol, 1)};
    QVERIFY(RideMetricFactory::instance().replaceUserMetrics(
        initial, RideMetric::userMetricFingerprint(initial)));

    TestAthleteContext firstContext{101};
    TestAthleteContext secondContext{202};
    RideItem firstItem;
    firstItem.context = asContext(&firstContext);
    RideItem secondItem;
    secondItem.context = asContext(&secondContext);

    std::atomic<bool> start{false};
    std::atomic<int> invalidResults{0};
    const auto evaluate = [&](RideItem *item, double expected) {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 1000; ++i) {
            IntervalItem interval;
            const auto result = RideMetric::computeMetrics(
                item, Specification(&interval, 1.0), {userSymbol});
            const RideMetricPtr metric = result.value(userSymbol);
            if (!metric || metric->value() != expected)
                invalidResults.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread firstEvaluator(evaluate, &firstItem, 101.0);
    std::thread secondEvaluator(evaluate, &secondItem, 202.0);

    start.store(true, std::memory_order_release);
    for (quint64 version = 2; version < 300; ++version) {
        QList<UserMetricSettings> replacement{
            settings(userSymbol, version)};
        QVERIFY(RideMetricFactory::instance().replaceUserMetrics(
            replacement,
            RideMetric::userMetricFingerprint(replacement)));
    }
    firstEvaluator.join();
    secondEvaluator.join();

    QCOMPARE(invalidResults.load(std::memory_order_relaxed), 0);
}

void TestUserMetricRegistrySafety::
repeatedReloadPreservesOrderingSchemaAndDependencies()
{
    for (quint64 version = 1; version <= 50; ++version) {
        QList<UserMetricSettings> userMetrics{
            settings(userSymbol, version),
            settings(secondUserSymbol, version + 100)};
        const quint16 schema =
            RideMetric::userMetricFingerprint(userMetrics);
        QVERIFY(RideMetricFactory::instance().replaceUserMetrics(
            userMetrics, schema));

        const RideMetricRegistrySnapshot registry =
            RideMetricFactory::instance().snapshot();
        QCOMPARE(registry.metricCount(), builtinCount_ + 2);
        QCOMPARE(registry.metricName(builtinCount_), userSymbol);
        QCOMPARE(registry.metricName(builtinCount_ + 1), secondUserSymbol);
        QCOMPARE(registry.rideMetric(userSymbol)->index(), builtinCount_);
        QCOMPARE(registry.rideMetric(secondUserSymbol)->index(),
                 builtinCount_ + 1);
        QCOMPARE(registry.userMetricSchemaVersion(), schema);
        QCOMPARE(UserMetricSchemaVersion.load(std::memory_order_acquire),
                 schema);

        IntervalItem interval;
        const auto result = RideMetric::computeMetrics(
            nullptr, Specification(&interval, 1.0),
            {dependencyRootSymbol}, registry);
        QCOMPARE(result.value(dependencyRootSymbol)->value(), 5.0);
    }
}

void TestUserMetricRegistrySafety::activeEvaluationDoesNotBlockPublication()
{
    {
        std::lock_guard<std::mutex> lock(blockingState_->mutex);
        blockingState_->entered = false;
        blockingState_->released = false;
    }

    std::atomic<bool> evaluated{false};
    std::thread evaluator([&] {
        IntervalItem interval;
        const auto result = RideMetric::computeMetrics(
            nullptr, Specification(&interval, 1.0), {blockingSymbol});
        evaluated.store(result.contains(blockingSymbol),
                        std::memory_order_release);
    });

    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(blockingState_->mutex);
        entered = blockingState_->condition.wait_for(
            lock, std::chrono::seconds(2),
            [this] { return blockingState_->entered; });
    }

    std::atomic<bool> published{false};
    std::thread publisher;
    if (entered) {
        publisher = std::thread([&] {
            QList<UserMetricSettings> replacement{settings(userSymbol, 900)};
            RideMetricFactory::instance().replaceUserMetrics(
                replacement, RideMetric::userMetricFingerprint(replacement));
            published.store(true, std::memory_order_release);
        });
    }
    QElapsedTimer timer;
    timer.start();
    while (entered && !published.load(std::memory_order_acquire)
           && timer.elapsed() < 2000) {
        QThread::msleep(1);
    }
    const bool evaluationStayedActive =
        !evaluated.load(std::memory_order_acquire);

    {
        std::lock_guard<std::mutex> lock(blockingState_->mutex);
        blockingState_->released = true;
    }
    blockingState_->condition.notify_all();
    if (publisher.joinable()) publisher.join();
    evaluator.join();
    QVERIFY(entered);
    QVERIFY(published.load(std::memory_order_acquire));
    QVERIFY(evaluationStayedActive);
    QVERIFY(evaluated.load(std::memory_order_acquire));
}

void TestUserMetricRegistrySafety::
clonesReuseContextFreeCompiledProgramAfterDefinitionTeardown()
{
    auto definition = std::make_unique<UserMetric>(settings(userSymbol, 1));
    DataFilter *program = definition->program.data();
    QVERIFY(program);
    QCOMPARE(program->context, nullptr);

    std::unique_ptr<UserMetric> first(
        static_cast<UserMetric *>(definition->clone()));
    std::unique_ptr<UserMetric> second(
        static_cast<UserMetric *>(definition->clone()));
    QCOMPARE(first->program.data(), program);
    QCOMPARE(second->program.data(), program);
    definition.reset();

    TestAthleteContext athleteContext{303};
    RideItem item;
    item.context = asContext(&athleteContext);
    IntervalItem interval;
    first->compute(&item, Specification(&interval, 1.0), {});
    second->compute(&item, Specification(&interval, 1.0), {});
    QCOMPARE(first->value(), 303.0);
    QCOMPARE(second->value(), 303.0);
}

void TestUserMetricRegistrySafety::removedUserMetricDefinitionsAreReclaimed()
{
    std::atomic<int> liveDefinitions{0};
    {
        ReloadMetric metric(userSymbol, 1, true, &liveDefinitions);
        QVERIFY(RideMetricFactory::instance().addMetric(metric));
    }
    QCOMPARE(liveDefinitions.load(), 1);

    RideMetricRegistrySnapshot retired =
        RideMetricFactory::instance().snapshot();
    RideMetricFactory::instance().removeUserMetrics();
    QCOMPARE(liveDefinitions.load(), 1);
    QVERIFY(retired.haveMetric(userSymbol));
    QVERIFY(!RideMetricFactory::instance().haveMetric(userSymbol));
    retired = RideMetricRegistrySnapshot();

    QCOMPARE(liveDefinitions.load(), 0);
}

QTEST_GUILESS_MAIN(TestUserMetricRegistrySafety)
#include "testUserMetricRegistrySafety.moc"
