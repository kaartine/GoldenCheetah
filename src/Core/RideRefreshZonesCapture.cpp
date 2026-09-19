/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshZones.h"

#include "HrZones.h"
#include "PaceZones.h"
#include "Zones.h"

#include <QCoreApplication>

RideRefreshZones::PowerHistory captureRideRefreshPowerZones(
    const Zones *source, int rawCpForFtpSetting)
{
    RideRefreshZones::PowerHistory result;
    if (!source) return result;
    result.present = true;
    result.sport = const_cast<Zones *>(source)->sport();
    result.cpForFtpSettingKey = source->useCPforFTPSetting();
    result.rawCpForFtpSetting = rawCpForFtpSetting;
    result.ranges.reserve(source->getRangeSize());
    for (int index = 0; index < source->getRangeSize(); ++index) {
        // Legacy getters return a value copy but are missing const qualifiers.
        const ZoneRange legacy = const_cast<Zones *>(source)->getZoneRange(index);
        RideRefreshZones::PowerRange range;
        range.begin = legacy.begin;
        range.end = legacy.end;
        range.cp = legacy.cp;
        range.rawAeT = legacy.aet;
        range.ftp = legacy.ftp;
        range.wprime = legacy.wprime;
        range.pmax = legacy.pmax;
        range.bands.reserve(legacy.zones.size());
        for (const ZoneInfo &zone : legacy.zones) {
            range.bands.append({
                zone.name, zone.desc, zone.lo, zone.hi, 0.0});
        }
        result.ranges.append(std::move(range));
    }
    return result;
}

RideRefreshZones::HeartRateHistory captureRideRefreshHeartRateZones(
    const HrZones *source)
{
    RideRefreshZones::HeartRateHistory result;
    if (!source) return result;
    result.present = true;
    result.sport = source->sport();
    result.ranges.reserve(source->getRangeSize());
    for (int index = 0; index < source->getRangeSize(); ++index) {
        const HrZoneRange legacy =
            const_cast<HrZones *>(source)->getHrZoneRange(index);
        RideRefreshZones::HeartRateRange range;
        range.begin = legacy.begin;
        range.end = legacy.end;
        range.lt = legacy.lt;
        range.rawAeT = legacy.aet;
        range.restHr = legacy.restHr;
        range.maxHr = legacy.maxHr;
        range.bands.reserve(legacy.zones.size());
        for (const HrZoneInfo &zone : legacy.zones) {
            range.bands.append({
                zone.name, zone.desc, zone.lo, zone.hi, zone.trimp});
        }
        result.ranges.append(std::move(range));
    }
    return result;
}

RideRefreshZones::PaceHistory captureRideRefreshPaceZones(
    const PaceZones *source,
    bool swim,
    bool metricPace)
{
    RideRefreshZones::PaceHistory result;
    result.swim = swim;
    result.metricUnits = QCoreApplication::translate(
        "PaceZones", swim ? "min/100m" : "min/km");
    result.imperialUnits = QCoreApplication::translate(
        "PaceZones", swim ? "min/100yd" : "min/mile");
    result.metricPace = metricPace;
    if (!source) return result;
    result.present = true;
    result.ranges.reserve(source->getRangeSize());
    for (int index = 0; index < source->getRangeSize(); ++index) {
        const PaceZoneRange legacy =
            const_cast<PaceZones *>(source)->getZoneRange(index);
        RideRefreshZones::PaceRange range;
        range.begin = legacy.begin;
        range.end = legacy.end;
        range.cv = legacy.cv;
        range.rawAeT = legacy.aet;
        range.bands.reserve(legacy.zones.size());
        for (const PaceZoneInfo &zone : legacy.zones) {
            range.bands.append({
                zone.name, zone.desc, zone.lo, zone.hi, 0.0});
        }
        result.ranges.append(std::move(range));
    }
    return result;
}
