/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshZones.h"

#include "PaceZones.h"
#include "Zones.h"

#include <QObject>
#include <QThread>

#include <utility>

namespace {

QVariant setting(
    const QHash<QString, QVariant> &settings,
    const QString &key,
    const QVariant &fallback)
{
    const QVariant captured = settings.value(key);
    return captured.isValid() ? captured : fallback;
}

} // namespace

std::shared_ptr<const RideRefreshZones>
captureRideRefreshZonesForOwner(
    const QObject *owner,
    const QHash<QString, Zones *> &powerZones,
    const QHash<QString, HrZones *> &heartRateZones,
    const PaceZones *run,
    const PaceZones *swim,
    const QHash<QString, QVariant> &globalSettings,
    const QHash<QString, QVariant> &athleteSettings,
    bool useMetricUnits)
{
    if (!owner || QThread::currentThread() != owner->thread()) return {};

    QHash<QString, RideRefreshZones::PowerHistory> power;
    for (auto it = powerZones.constBegin(); it != powerZones.constEnd(); ++it) {
        const Zones *source = it.value();
        const QString key = source ? source->useCPforFTPSetting() : QString();
        power.insert(it.key(), captureRideRefreshPowerZones(
            source, setting(athleteSettings, key, 0).toInt()));
    }

    QHash<QString, RideRefreshZones::HeartRateHistory> heartRate;
    for (auto it = heartRateZones.constBegin();
         it != heartRateZones.constEnd(); ++it) {
        heartRate.insert(
            it.key(), captureRideRefreshHeartRateZones(it.value()));
    }

    return RideRefreshZones::create(
        std::move(power),
        std::move(heartRate),
        captureRideRefreshPaceZones(
            run,
            false,
            setting(
                globalSettings,
                run ? run->paceSetting()
                    : QStringLiteral("<global-general>pace"),
                useMetricUnits).toBool()),
        captureRideRefreshPaceZones(
            swim,
            true,
            setting(
                globalSettings,
                swim ? swim->paceSetting()
                     : QStringLiteral("<global-general>swimpace"),
                useMetricUnits).toBool()));
}
