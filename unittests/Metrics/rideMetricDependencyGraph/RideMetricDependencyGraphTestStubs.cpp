/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "IntervalItem.h"
#include "Specification.h"
#include "TimeUtils.h"

RideMetric *createUserMetricForRegistry(UserMetricSettings)
{
    return nullptr;
}

RideFile *RideItem::ride(bool)
{
    return nullptr;
}

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

IntervalItem::IntervalItem()
    : rideItem_(nullptr), selected(false), name(""),
      type(RideFileInterval::USER), start(0), stop(0), startKM(0), stopKM(0),
      displaySequence(0), color(Qt::black), test(false), rideInterval(nullptr)
{
    metrics_.fill(0, RideMetricFactory::instance().metricCount());
    count_.fill(0, RideMetricFactory::instance().metricCount());
}
