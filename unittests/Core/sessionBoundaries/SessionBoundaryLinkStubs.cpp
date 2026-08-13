/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "CompareDateRange.h"
#include "CompareInterval.h"
#include "RealtimeData.h"
#include "RideItem.h"
#include "TimeUtils.h"

#include <cstring>
#include <utility>

DateRange::DateRange(
    QDate from,
    QDate to,
    QString name,
    QColor color)
    : from(from)
    , to(to)
    , name(std::move(name))
    , color(color)
    , valid(from.isValid() && to.isValid())
{
}

DateRange::DateRange(const DateRange &other)
    : from(other.from)
    , to(other.to)
    , name(other.name)
    , color(other.color)
    , id(other.id)
    , valid(other.valid)
{
}

DateRange &DateRange::operator=(const DateRange &other)
{
    from = other.from;
    to = other.to;
    name = other.name;
    color = other.color;
    id = other.id;
    valid = other.valid;
    return *this;
}

CompareInterval::~CompareInterval() = default;
CompareDateRange::~CompareDateRange() = default;

RealtimeData::RealtimeData()
    : mode(ErgFileFormat::unknown)
{
    std::memset(name, 0, sizeof(name));
    std::memset(spinScan, 0, sizeof(spinScan));
    hr = watts = altWatts = altDistance = speed = wheelRpm = load = slope =
        lrbalance = cadence = smo2 = thb = lte = rte = lps = rps = 0.0;
    rppb = rppe = rpppb = rpppe = lppb = lppe = lpppb = lpppe = 0.0;
    rightPCO = leftPCO = torque = RTorque = LTorque = 0.0;
    latitude = longitude = altitude = 0.0;
    vo2 = vco2 = rf = rmv = tv = feo2 = 0.0;
    position = RealtimeData::seated;
    temp = skinTemp = coreTemp = heatStrain = 0.0;
    wheelRpmSampleTime = {};
    distance = routeDistance = distanceRemaining = 0.0;
    lapDistance = lapDistanceRemaining = virtualSpeed = wbal = 0.0;
    hhb = o2hb = rer = 0.0;
    lap = msecs = lapMsecs = lapMsecsRemaining = ergMsecsRemaining = 0;
    trainerStatusAvailable = false;
    trainerReady = trainerRunning = true;
    trainerCalibRequired = trainerConfigRequired = trainerBrakeFault = false;
}

RideItem::RideItem() = default;
RideItem::~RideItem() = default;
void RideItem::modified() {}
void RideItem::reverted() {}
void RideItem::saved() {}
void RideItem::notifyRideDataChanged() {}
void RideItem::notifyRideMetadataChanged() {}
