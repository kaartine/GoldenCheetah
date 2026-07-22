/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "Settings.h"
#include "TimeUtils.h"

#include <QTimeZone>

namespace {

int testSmartRecording = Qt::Checked;
int testHighWaterMark = 25;

} // namespace

void setTcxPointBudgetTestSettings(int smartRecording, int highWaterMark)
{
    testSmartRecording = smartRecording;
    testHighWaterMark = highWaterMark;
}

GSettings::GSettings(QString, QString)
{
}

GSettings::GSettings(QString, QSettings::Format)
{
}

GSettings::~GSettings()
{
}

QVariant GSettings::value(const QObject *, const QString key,
                          const QVariant def)
{
    if (key == QLatin1String(GC_GARMIN_SMARTRECORD))
        return testSmartRecording;
    if (key == QLatin1String(GC_GARMIN_HWMARK))
        return testHighWaterMark;
    return def;
}

static GSettings testSettings(QStringLiteral("GoldenCheetah"),
                              QStringLiteral("TcxPointBudgetTest"));
GSettings *appsettings = &testSettings;
int OperatingSystem = LINUX;

QDateTime convertToLocalTime(QString timestamp)
{
    QDateTime value = QDateTime::fromString(timestamp, Qt::ISODate);
    if (timestamp.endsWith(QLatin1Char('Z'), Qt::CaseInsensitive))
        value.setTimeZone(QTimeZone::UTC);
    return value.toLocalTime();
}

namespace Utils {

QString RidefileUnEscape(QString value)
{
    value.replace(QStringLiteral("\\t"), QStringLiteral("\t"));
    value.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    value.replace(QStringLiteral("\\r"), QStringLiteral("\r"));
    value.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
    return value;
}

} // namespace Utils
