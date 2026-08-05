/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "DataFilter.h"
#include "Context.h"
#include "IntervalItem.h"
#include "RideItem.h"
#include "RealtimeData.h"
#include "Specification.h"
#include "TimeUtils.h"

#include "UserMetricRegistrySafetyTestTypes.h"

namespace {

Leaf *functionLeaf(const QString &name)
{
    Leaf *leaf = new Leaf(0, 0);
    leaf->function = name;
    return leaf;
}

} // namespace

DataFilter::DataFilter(QObject *parent, Context *context)
    : QObject(parent), context(context), treeRoot(new Leaf(0, 0)),
      list(nullptr), parent_(parent)
{
    rt.owner = this;
    rt.chart = nullptr;
    rt.isdynamic = false;
}

DataFilter::DataFilter(QObject *parent, Context *context, QString formula)
    : QObject(parent), context(context), treeRoot(new Leaf(0, 0)),
      list(nullptr), sig(std::move(formula)), parent_(parent)
{
    rt.owner = this;
    rt.chart = nullptr;
    rt.isdynamic = false;
    rt.functions.insert(QStringLiteral("relevant"),
                        functionLeaf(QStringLiteral("relevant")));
    rt.functions.insert(QStringLiteral("value"),
                        functionLeaf(QStringLiteral("value")));
    rt.functions.insert(QStringLiteral("count"),
                        functionLeaf(QStringLiteral("count")));
}

DataFilter::~DataFilter()
{
    clearFilter();
}

void DataFilter::clearFilter()
{
    qDeleteAll(rt.functions);
    rt.functions.clear();
    delete treeRoot;
    treeRoot = nullptr;
}

QStringList DataFilter::parseFilter(Context *, QString, QStringList *)
{
    return {};
}

QStringList DataFilter::check(QString)
{
    return {};
}

void DataFilter::configChanged(qint32)
{
}

void DataFilter::dynamicParse()
{
}

Result Leaf::eval(DataFilterRuntime *, Leaf *function, const Result &, long,
                  RideItem *item, RideFilePoint *,
                  const QHash<QString, RideMetric *> *,
                  const Specification &, const DateRange &)
{
    if (!function || function->function == QStringLiteral("relevant"))
        return Result(1.0);
    if (function->function == QStringLiteral("count"))
        return Result(1.0);
    if (function->function == QStringLiteral("value")
        && item && item->context) {
        return Result(double(asTestContext(item->context)->value));
    }
    return Result(0.0);
}

void Leaf::clear(Leaf *)
{
}

RideItem::RideItem()
    : ride_(nullptr), fileCache_(nullptr), context(nullptr), isdirty(false),
      isstale(false), isedit(false), skipsave(false)
{
}

RideItem::~RideItem()
{
}

RideFile *RideItem::ride(bool)
{
    return nullptr;
}

void RideItem::modified() {}
void RideItem::reverted() {}
void RideItem::saved() {}
void RideItem::notifyRideDataChanged() {}
void RideItem::notifyRideMetadataChanged() {}

QString time_to_string(double, bool)
{
    return QString();
}

DateRange::DateRange(QDate from, QDate to, QString name, QColor color)
    : from(from), to(to), name(std::move(name)), color(color),
      valid(from.isValid() && to.isValid())
{
}

DateRange::DateRange(const DateRange &other)
    : from(other.from), to(other.to), name(other.name), color(other.color),
      id(other.id), valid(from.isValid() && to.isValid())
{
}

DateRange &DateRange::operator=(const DateRange &other)
{
    from = other.from;
    to = other.to;
    name = other.name;
    color = other.color;
    id = other.id;
    valid = from.isValid() && to.isValid();
    return *this;
}

PlanFilter::PlanFilter(PlanFilterType type)
    : type(type)
{
}

Specification::Specification(IntervalItem *interval, double recintsecs)
    : it(interval), recintsecs(recintsecs), ri(nullptr)
{
}

Specification::Specification()
    : it(nullptr), recintsecs(0), ri(nullptr)
{
}

bool Specification::isEmpty(RideFile *) const
{
    return true;
}

RideFileIterator::RideFileIterator(
    RideFile *ride, Specification, IterationSpec)
    : f(ride), start(0), stop(0), index(0)
{
}

bool RideFileIterator::hasNext() const
{
    return false;
}

RideFilePoint *RideFileIterator::next()
{
    return nullptr;
}

IntervalItem::IntervalItem()
    : rideItem_(nullptr), selected(false), name(""),
      type(RideFileInterval::USER), start(0), stop(0), startKM(0), stopKM(0),
      displaySequence(0), color(Qt::black), test(false), rideInterval(nullptr)
{
    metrics_.fill(0, RideMetricFactory::instance().metricCount());
    count_.fill(0, RideMetricFactory::instance().metricCount());
}

GlobalContext *GlobalContext::context()
{
    return nullptr;
}

void GlobalContext::readConfig(qint32) {}
void GlobalContext::userMetricsConfigChanged() {}

void Context::notifyConfigChanged(qint32) {}
void Context::notifyCompareIntervals(bool) {}
void Context::notifyCompareIntervalsChanged() {}
void Context::notifyCompareDateRanges(bool) {}
void Context::notifyCompareDateRangesChanged() {}

RealtimeData::RealtimeData() {}

void CP2Model::onDataChanged() {}
void CP2Model::onIntervalsChanged() {}
void CP3Model::onDataChanged() {}
void CP3Model::onIntervalsChanged() {}
void WSModel::onDataChanged() {}
void WSModel::onIntervalsChanged() {}
void MultiModel::onDataChanged() {}
void MultiModel::onIntervalsChanged() {}
void ExtendedModel::onDataChanged() {}
void ExtendedModel::onIntervalsChanged() {}
