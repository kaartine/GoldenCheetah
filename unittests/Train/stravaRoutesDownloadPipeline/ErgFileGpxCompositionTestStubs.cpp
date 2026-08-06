/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Core/Settings.h"
#include "Metrics/Zones.h"

#include <QThread>

#include <atomic>

std::atomic_bool gpxSettingsReadBlocked{false};
std::atomic_bool gpxSettingsReadEntered{false};
std::atomic_int gpxSettingsReadCalls{0};

GSettings::GSettings(QString, QString)
    : newFormat(true)
{
}

GSettings::~GSettings() = default;

QVariant GSettings::value(
    const QObject *, const QString, const QVariant defaultValue)
{
    gpxSettingsReadCalls.fetch_add(1, std::memory_order_release);
    gpxSettingsReadEntered.store(true, std::memory_order_release);
    while (gpxSettingsReadBlocked.load(std::memory_order_acquire))
        QThread::msleep(1);
    return defaultValue;
}

QVariant GSettings::cvalue(
    QString, QString, QVariant defaultValue)
{
    return defaultValue;
}

namespace {
GSettings testSettings(
    QStringLiteral("GoldenCheetah"),
    QStringLiteral("StravaRoutesDownloadPipelineTest"));
}

GSettings *appsettings = &testSettings;

int Zones::whichRange(const QDate &) const
{
    return -1;
}

int Zones::getCP(int) const
{
    return 300;
}

QList<int> Zones::getZoneHighs(int) const
{
    return {};
}
