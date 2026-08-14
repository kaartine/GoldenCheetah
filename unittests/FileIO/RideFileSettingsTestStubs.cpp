/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Settings.h"

#include <QDir>

GSettings::GSettings(QString, QString)
{
}

GSettings::GSettings(QString, QSettings::Format)
{
}

GSettings::~GSettings()
{
}

QVariant GSettings::value(
    const QObject *, const QString, const QVariant defaultValue)
{
    return defaultValue;
}

QVariant GSettings::cvalue(
    QString, QString, QVariant defaultValue)
{
    return defaultValue;
}

static GSettings testSettings(
    QStringLiteral("GoldenCheetah"),
    QStringLiteral("RideFileLinkTest"));
GSettings *appsettings = &testSettings;
int OperatingSystem = LINUX;
QString gcroot = QDir::tempPath();
