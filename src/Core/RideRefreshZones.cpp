/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshZones.h"

#include "Units.h"

#include <QByteArray>

#include <cmath>
#include <utility>

namespace {

template<typename Range>
int whichRange(const QVector<Range> &ranges, const QDate &date)
{
    for (int index = 0; index < ranges.size(); ++index) {
        const Range &range = ranges.at(index);
        if ((date >= range.begin || range.begin.isNull())
            && (date < range.end || range.end.isNull())) {
            return index;
        }
    }
    return -1;
}

template<typename T>
int whichZone(const QVector<RideRefreshZones::Band<T>> &bands, double value)
{
    for (int index = 0; index < bands.size(); ++index) {
        const auto &band = bands.at(index);
        if (value >= band.low && value < band.high)
            return index;
    }
    return -1;
}

quint16 checksum(quint64 value)
{
    return qChecksum(QByteArray::number(value));
}

} // namespace

int RideRefreshZones::PowerRange::resolvedAeT() const
{
    return rawAeT > 0 ? rawAeT : int(std::round(0.85 * cp));
}

int RideRefreshZones::PowerRange::whichZone(double value) const
{
    return ::whichZone(bands, value);
}

quint16 RideRefreshZones::PowerRange::fingerprint() const
{
    quint64 value = quint64(cp) + quint64(rawAeT) + quint64(ftp)
        + quint64(wprime) + quint64(pmax);
    for (const Band<int> &band : bands) value += quint64(band.low);
    return checksum(value);
}

int RideRefreshZones::HeartRateRange::resolvedAeT() const
{
    return rawAeT > 0 ? rawAeT : int(std::round(0.9 * lt));
}

int RideRefreshZones::HeartRateRange::whichZone(double value) const
{
    return ::whichZone(bands, value);
}

quint16 RideRefreshZones::HeartRateRange::fingerprint() const
{
    quint64 value = quint64(lt) + quint64(rawAeT) + quint64(restHr)
        + quint64(maxHr);
    for (const Band<int> &band : bands) {
        value += quint64(band.low);
        value += quint64(int(100.0f * band.trimp));
    }
    return checksum(value);
}

double RideRefreshZones::PaceRange::resolvedAeT(bool swim) const
{
    if (rawAeT > 0.0) return rawAeT;
    return (swim ? 0.975 : 0.9) * cv;
}

int RideRefreshZones::PaceRange::whichZone(double value) const
{
    return ::whichZone(bands, value);
}

quint16 RideRefreshZones::PaceRange::fingerprint() const
{
    quint64 value = quint64(int(100.0f * cv))
        + quint64(int(100.0f * rawAeT));
    for (const Band<double> &band : bands)
        value += quint64(int(100.0f * band.low));
    return checksum(value);
}

int RideRefreshZones::PowerHistory::whichRange(const QDate &date) const
{
    return ::whichRange(ranges, date);
}

const RideRefreshZones::PowerRange *
RideRefreshZones::PowerHistory::rangeForDate(const QDate &date) const
{
    const int index = whichRange(date);
    return index >= 0 ? &ranges.at(index) : nullptr;
}

quint16 RideRefreshZones::PowerHistory::fingerprint(const QDate &date) const
{
    const PowerRange *range = rangeForDate(date);
    return range ? range->fingerprint() : checksum(0);
}

int RideRefreshZones::HeartRateHistory::whichRange(const QDate &date) const
{
    return ::whichRange(ranges, date);
}

const RideRefreshZones::HeartRateRange *
RideRefreshZones::HeartRateHistory::rangeForDate(const QDate &date) const
{
    const int index = whichRange(date);
    return index >= 0 ? &ranges.at(index) : nullptr;
}

quint16 RideRefreshZones::HeartRateHistory::fingerprint(
    const QDate &date) const
{
    const HeartRateRange *range = rangeForDate(date);
    return range ? range->fingerprint() : checksum(0);
}

int RideRefreshZones::PaceHistory::whichRange(const QDate &date) const
{
    return ::whichRange(ranges, date);
}

const RideRefreshZones::PaceRange *
RideRefreshZones::PaceHistory::rangeForDate(const QDate &date) const
{
    const int index = whichRange(date);
    return index >= 0 ? &ranges.at(index) : nullptr;
}

quint16 RideRefreshZones::PaceHistory::fingerprint(const QDate &date) const
{
    const PaceRange *range = rangeForDate(date);
    return range ? range->fingerprint() : checksum(0);
}

QString RideRefreshZones::PaceHistory::formatPace(double kph) const
{
    return kphToPace(kph, metricPace, swim);
}

QString RideRefreshZones::PaceHistory::paceUnits() const
{
    return metricPace ? metricUnits : imperialUnits;
}

std::shared_ptr<const RideRefreshZones> RideRefreshZones::create(
    QHash<QString, PowerHistory> power,
    QHash<QString, HeartRateHistory> heartRate,
    PaceHistory run,
    PaceHistory swim)
{
    run.swim = false;
    swim.swim = true;
    return std::shared_ptr<const RideRefreshZones>(new RideRefreshZones(
        std::move(power), std::move(heartRate),
        std::move(run), std::move(swim)));
}

RideRefreshZones::RideRefreshZones(
    QHash<QString, PowerHistory> power,
    QHash<QString, HeartRateHistory> heartRate,
    PaceHistory run,
    PaceHistory swim)
    : power_(std::move(power)),
      heartRate_(std::move(heartRate)),
      run_(std::move(run)),
      swim_(std::move(swim))
{
}

const RideRefreshZones::PowerHistory *
RideRefreshZones::power(const QString &sport) const
{
    auto found = power_.constFind(sport);
    if (found != power_.constEnd())
        return found->present ? &found.value() : nullptr;
    found = power_.constFind(QStringLiteral("Bike"));
    return found != power_.constEnd() && found->present
        ? &found.value() : nullptr;
}

const RideRefreshZones::HeartRateHistory *
RideRefreshZones::heartRate(const QString &sport) const
{
    auto found = heartRate_.constFind(sport);
    if (found != heartRate_.constEnd())
        return found->present ? &found.value() : nullptr;
    found = heartRate_.constFind(QStringLiteral("Bike"));
    return found != heartRate_.constEnd() && found->present
        ? &found.value() : nullptr;
}
