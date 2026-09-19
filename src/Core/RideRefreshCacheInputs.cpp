/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshCacheInputs.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QList>

#include <cmath>

namespace {

template<typename T>
QList<T> lows(const QVector<RideRefreshZones::Band<T>> &bands)
{
    QList<T> values;
    values.reserve(bands.size());
    for (const auto &band : bands) values.append(band.low);
    return values;
}

template<typename T>
QList<T> highs(const QVector<RideRefreshZones::Band<T>> &bands)
{
    QList<T> values;
    values.reserve(bands.size());
    for (const auto &band : bands) values.append(band.high);
    return values;
}

} // namespace

QByteArray rideRefreshCacheAnalysisFingerprint(
    const RideRefreshZones *zones,
    const RideRefreshCacheSettings &settings,
    const QDate &date,
    const QString &sport,
    bool isSwim,
    double weight)
{
    if (!zones || !std::isfinite(weight) || weight <= 0.0) return {};

    QByteArray canonical;
    QDataStream stream(&canonical, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_4_6);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(1)
           << qint64(date.toJulianDay())
           << sport
           << isSwim
           << weight;

    const auto *power = zones->power(sport);
    const int powerRange = power ? power->whichRange(date) : -1;
    stream << bool(power) << qint32(powerRange);
    if (powerRange >= 0) {
        const auto &range = power->ranges.at(powerRange);
        stream << qint32(range.cp)
               << qint32(range.resolvedAeT())
               << qint32(range.ftp)
               << qint32(range.wprime)
               << qint32(range.pmax)
               << lows(range.bands)
               << highs(range.bands);
    }

    const auto *heartRate = zones->heartRate(sport);
    const int heartRateRange = heartRate
        ? heartRate->whichRange(date) : -1;
    stream << bool(heartRate) << qint32(heartRateRange);
    if (heartRateRange >= 0) {
        const auto &range = heartRate->ranges.at(heartRateRange);
        stream << qint32(range.lt)
               << qint32(range.resolvedAeT())
               << qint32(range.restHr)
               << qint32(range.maxHr)
               << lows(range.bands)
               << highs(range.bands);
    }

    const auto *pace = zones->pace(isSwim);
    const int paceRange = pace ? pace->whichRange(date) : -1;
    stream << bool(pace) << qint32(paceRange);
    if (paceRange >= 0) {
        const auto &range = pace->ranges.at(paceRange);
        stream << range.cv
               << range.resolvedAeT(isSwim)
               << lows(range.bands)
               << highs(range.bands);
    }

    stream << settings.wbalFormula
           << qint32(settings.wbalTau)
           << qint32(settings.wheelSize);
    if (stream.status() != QDataStream::Ok) return {};
    return QCryptographicHash::hash(
        canonical, QCryptographicHash::Sha256);
}

RideRefreshDistributionZones rideRefreshDistributionZones(
    const RideRefreshZones *zones,
    const QDate &date,
    const QString &sport,
    bool isSwim)
{
    RideRefreshDistributionZones result;
    if (!zones) return result;
    result.valid = true;

    if (const auto *power = zones->power(sport)) {
        result.powerDomainPresent = true;
        if (const auto *range = power->rangeForDate(date))
            result.power = *range;
    }
    if (const auto *heartRate = zones->heartRate(sport)) {
        result.heartRateDomainPresent = true;
        if (const auto *range = heartRate->rangeForDate(date))
            result.heartRate = *range;
    }
    if (const auto *pace = zones->pace(isSwim)) {
        result.paceDomainPresent = true;
        if (const auto *range = pace->rangeForDate(date))
            result.pace = *range;
    }
    return result;
}
