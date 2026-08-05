/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <QtTest>

#include "IntervalItem.h"
#include "RideMetric.h"
#include "Specification.h"

namespace {

struct MetricTrace
{
    QStringList computeOrder;
    QHash<QString, int> computeCounts;
    int liveComputeInstances = 0;
};

class GraphMetric final : public RideMetric
{
public:
    enum class Role { Definition, Registered, ComputeInstance };

    GraphMetric(QString symbol, double baseValue, QVector<QString> dependencies,
                MetricTrace *trace, bool nullComputeClone = false)
        : dependencies_(std::move(dependencies)), trace_(trace),
          baseValue_(baseValue), nullComputeClone_(nullComputeClone)
    {
        setSymbol(std::move(symbol));
    }

    GraphMetric(const GraphMetric &other, Role role)
        : RideMetric(other), dependencies_(other.dependencies_),
          trace_(other.trace_), baseValue_(other.baseValue_),
          nullComputeClone_(other.nullComputeClone_), role_(role)
    {
        if (role_ == Role::ComputeInstance) ++trace_->liveComputeInstances;
    }

    ~GraphMetric() override
    {
        if (role_ == Role::ComputeInstance) --trace_->liveComputeInstances;
    }

    RideMetric *clone() const override
    {
        if (role_ == Role::Definition)
            return new GraphMetric(*this, Role::Registered);
        if (nullComputeClone_)
            return nullptr;
        return new GraphMetric(*this, Role::ComputeInstance);
    }

    void compute(RideItem *, Specification,
                 const QHash<QString, RideMetric *> &computed) override
    {
        trace_->computeOrder.append(symbol());
        ++trace_->computeCounts[symbol()];

        double result = baseValue_;
        for (const QString &dependency : dependencies_) {
            RideMetric *metric = computed.value(dependency, nullptr);
            if (metric) result += metric->value();
        }
        setValue(result);
    }

private:
    QVector<QString> dependencies_;
    MetricTrace *trace_;
    double baseValue_;
    bool nullComputeClone_;
    Role role_ = Role::Definition;
};

QString symbol(const char *test, const char *node)
{
    return QStringLiteral("metric001_%1_%2").arg(
        QString::fromLatin1(test), QString::fromLatin1(node));
}

bool containsOnlyNonNullMetrics(const QHash<QString, RideMetricPtr> &metrics)
{
    for (auto it = metrics.cbegin(); it != metrics.cend(); ++it)
        if (it.value().isNull()) return false;
    return true;
}

} // namespace

class TestRideMetricDependencyGraph : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void missingDependencySkipsUnresolvableBranch();
    void selfCycleTerminatesWithoutComputingCycle();
    void multiNodeCycleTerminatesWithoutComputingDependents();
    void diamondComputesSharedDependencyOnceInStableOrder();
    void validGraphComputesDependenciesBeforeRequestedMetric();
    void nullCloneAndItsDependentsAreOmitted();
    void duplicateRequestsComputeAndOwnOneInstance();

private:
    void addMetric(const QString &symbol, double baseValue,
                   const QVector<QString> &dependencies = {},
                   bool nullComputeClone = false);
    QHash<QString, RideMetricPtr> compute(const QStringList &requested);

    MetricTrace trace_;
};

void TestRideMetricDependencyGraph::init()
{
    trace_ = MetricTrace();
}

void TestRideMetricDependencyGraph::addMetric(
    const QString &metricSymbol, double baseValue,
    const QVector<QString> &dependencies, bool nullComputeClone)
{
    GraphMetric metric(metricSymbol, baseValue, dependencies, &trace_,
                       nullComputeClone);
    QVERIFY2(RideMetricFactory::instance().addMetric(metric, &dependencies),
             qPrintable(QStringLiteral("duplicate test metric: %1")
                            .arg(metricSymbol)));
}

QHash<QString, RideMetricPtr>
TestRideMetricDependencyGraph::compute(const QStringList &requested)
{
    IntervalItem interval;
    return RideMetric::computeMetrics(
        nullptr, Specification(&interval, 1.0), requested);
}

void TestRideMetricDependencyGraph::missingDependencySkipsUnresolvableBranch()
{
    const QString child = symbol("missing", "child");
    const QString parent = symbol("missing", "parent");
    const QString absent = symbol("missing", "absent");
    const QString independent = symbol("missing", "independent");
    addMetric(child, 1.0);
    addMetric(parent, 2.0, {child, absent});
    addMetric(independent, 3.0);

    auto result = compute({parent, independent});

    QCOMPARE(result.keys(), QStringList{independent});
    QVERIFY(containsOnlyNonNullMetrics(result));
    QCOMPARE(trace_.computeOrder, QStringList{independent});
    QCOMPARE(trace_.computeCounts.value(child), 0);
    QCOMPARE(trace_.computeCounts.value(parent), 0);
    QCOMPARE(trace_.liveComputeInstances, 1);
    result.clear();
    QCOMPARE(trace_.liveComputeInstances, 0);
}

void TestRideMetricDependencyGraph::selfCycleTerminatesWithoutComputingCycle()
{
    const QString cycle = symbol("self", "cycle");
    const QString independent = symbol("self", "independent");
    addMetric(cycle, 1.0, {cycle});
    addMetric(independent, 2.0);

    auto result = compute({cycle, independent});

    QCOMPARE(result.keys(), QStringList{independent});
    QVERIFY(containsOnlyNonNullMetrics(result));
    QCOMPARE(trace_.computeOrder, QStringList{independent});
    QCOMPARE(trace_.computeCounts.value(cycle), 0);
}

void TestRideMetricDependencyGraph::multiNodeCycleTerminatesWithoutComputingDependents()
{
    const QString a = symbol("multi", "a");
    const QString b = symbol("multi", "b");
    const QString c = symbol("multi", "c");
    const QString parent = symbol("multi", "parent");
    const QString independent = symbol("multi", "independent");
    addMetric(a, 1.0, {b});
    addMetric(b, 2.0, {c});
    addMetric(c, 3.0, {a});
    addMetric(parent, 4.0, {a});
    addMetric(independent, 5.0);

    auto result = compute({parent, a, independent});

    QCOMPARE(result.keys(), QStringList{independent});
    QVERIFY(containsOnlyNonNullMetrics(result));
    QCOMPARE(trace_.computeOrder, QStringList{independent});
    QCOMPARE(trace_.computeCounts.value(a), 0);
    QCOMPARE(trace_.computeCounts.value(b), 0);
    QCOMPARE(trace_.computeCounts.value(c), 0);
    QCOMPARE(trace_.computeCounts.value(parent), 0);
}

void TestRideMetricDependencyGraph::diamondComputesSharedDependencyOnceInStableOrder()
{
    const QString leaf = symbol("diamond", "leaf");
    const QString left = symbol("diamond", "left");
    const QString right = symbol("diamond", "right");
    const QString root = symbol("diamond", "root");
    addMetric(leaf, 1.0);
    addMetric(left, 10.0, {leaf});
    addMetric(right, 20.0, {leaf});
    addMetric(root, 100.0, {left, right});

    auto result = compute({root});

    QCOMPARE(result.keys(), QStringList{root});
    QVERIFY(containsOnlyNonNullMetrics(result));
    QCOMPARE(trace_.computeOrder, QStringList({leaf, left, right, root}));
    QCOMPARE(trace_.computeCounts.value(leaf), 1);
    QCOMPARE(result.value(root)->value(), 132.0);
    QCOMPARE(trace_.liveComputeInstances, 1);
    result.clear();
    QCOMPARE(trace_.liveComputeInstances, 0);
}

void TestRideMetricDependencyGraph::validGraphComputesDependenciesBeforeRequestedMetric()
{
    const QString first = symbol("valid", "first");
    const QString second = symbol("valid", "second");
    const QString root = symbol("valid", "root");
    addMetric(first, 2.0);
    addMetric(second, 3.0, {first});
    addMetric(root, 5.0, {second});

    auto result = compute({root});

    QCOMPARE(result.keys(), QStringList{root});
    QVERIFY(containsOnlyNonNullMetrics(result));
    QCOMPARE(trace_.computeOrder, QStringList({first, second, root}));
    QCOMPARE(result.value(root)->value(), 10.0);
}

void TestRideMetricDependencyGraph::nullCloneAndItsDependentsAreOmitted()
{
    const QString nullMetric = symbol("null", "clone");
    const QString parent = symbol("null", "parent");
    const QString independent = symbol("null", "independent");
    addMetric(nullMetric, 1.0, {}, true);
    addMetric(parent, 2.0, {nullMetric});
    addMetric(independent, 3.0);

    auto result = compute({nullMetric, parent, independent});

    QCOMPARE(result.keys(), QStringList{independent});
    QVERIFY(containsOnlyNonNullMetrics(result));
    QCOMPARE(trace_.computeOrder, QStringList{independent});
    QCOMPARE(trace_.computeCounts.value(parent), 0);
}

void TestRideMetricDependencyGraph::duplicateRequestsComputeAndOwnOneInstance()
{
    const QString metric = symbol("duplicate", "metric");
    addMetric(metric, 7.0);

    auto result = compute({metric, metric});

    QCOMPARE(result.keys(), QStringList{metric});
    QVERIFY(containsOnlyNonNullMetrics(result));
    QCOMPARE(trace_.computeCounts.value(metric), 1);
    QCOMPARE(trace_.liveComputeInstances, 1);
    result.clear();
    QCOMPARE(trace_.liveComputeInstances, 0);
}

QTEST_GUILESS_MAIN(TestRideMetricDependencyGraph)
#include "testRideMetricDependencyGraph.moc"
