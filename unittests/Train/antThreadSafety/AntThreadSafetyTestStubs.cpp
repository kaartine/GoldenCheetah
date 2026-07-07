/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "DeviceConfiguration.h"
#include "Settings.h"

int OperatingSystem = LINUX;

QVariant TestSettings::value(const QObject *, const QString &,
                             const QVariant &fallback)
{
    return fallback;
}

void TestSettings::setValue(const QString &, const QVariant &)
{
}

QVariant TestSettings::cvalue(const QString &, const QString &,
                              const QVariant &fallback)
{
    return fallback;
}

static TestSettings testSettings;
TestSettings *appsettings = &testSettings;

DeviceConfiguration::DeviceConfiguration()
    : type(0),
      wheelSize(2100),
      inertialMomentKGM2(0.0),
      stridelength(0),
      postProcess(0),
      controller(nullptr)
{
}
