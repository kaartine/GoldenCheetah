/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshZones.h"

#include "Athlete.h"
#include "PaceZones.h"
#include "Zones.h"

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
captureRideRefreshZones(
    const Athlete *athlete,
    const QHash<QString, QVariant> &globalSettings,
    const QHash<QString, QVariant> &athleteSettings,
    bool useMetricUnits)
{
    if (!athlete || QThread::currentThread() != athlete->thread()) return {};

    QHash<QString, RideRefreshZones::PowerHistory> power;
    for (auto it = athlete->zones_.constBegin();
         it != athlete->zones_.constEnd(); ++it) {
        const Zones *source = it.value();
        const QString key = source
            ? source->useCPforFTPSetting() : QString();
        power.insert(it.key(), captureRideRefreshPowerZones(
            source, setting(athleteSettings, key, 0).toInt()));
    }

    QHash<QString, RideRefreshZones::HeartRateHistory> heartRate;
    for (auto it = athlete->hrzones_.constBegin();
         it != athlete->hrzones_.constEnd(); ++it) {
        heartRate.insert(
            it.key(), captureRideRefreshHeartRateZones(it.value()));
    }

    const PaceZones *run = athlete->paceZones(false);
    const PaceZones *swim = athlete->paceZones(true);
    return RideRefreshZones::create(
        std::move(power), std::move(heartRate),
        captureRideRefreshPaceZones(
            run, false,
            setting(
                globalSettings,
                run ? run->paceSetting()
                    : QStringLiteral("<global-general>pace"),
                useMetricUnits).toBool()),
        captureRideRefreshPaceZones(
            swim, true,
            setting(
                globalSettings,
                swim ? swim->paceSetting()
                     : QStringLiteral("<global-general>swimpace"),
                useMetricUnits).toBool()));
}
