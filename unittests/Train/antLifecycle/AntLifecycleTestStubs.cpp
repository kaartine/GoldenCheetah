/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "GarminServiceHelper.h"
#include "RemoteControl.h"
#include "Settings.h"
#include "TrainSidebar.h"

int OperatingSystem = LINUX;

DeviceConfiguration::DeviceConfiguration() :
    type(0),
    wheelSize(2100),
    inertialMomentKGM2(0.0),
    stridelength(0),
    postProcess(0),
    controller(nullptr)
{
}

GSettings::GSettings(QString, QString) :
    newFormat(false),
    systemsettings(nullptr),
    oldsystemsettings(nullptr),
    global(nullptr)
{
}

GSettings::~GSettings()
{
}

QVariant GSettings::value(const QObject *, const QString, const QVariant def)
{
    return def;
}

void GSettings::setValue(QString, QVariant)
{
}

QVariant GSettings::cvalue(QString, QString, QVariant def)
{
    return def;
}

static GSettings testSettings(QStringLiteral("test"), QStringLiteral("test"));
GSettings *appsettings = &testSettings;

bool GarminServiceHelper::isServiceRunning()
{
    return false;
}

bool GarminServiceHelper::stopService()
{
    return true;
}

void TrainSidebar::Stop(int)
{
}

void TrainSidebar::Disconnect()
{
}

int RemoteControl::getNativeCmdId(int command) const
{
    return command;
}
