/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "DataProcessor.h"
#include "Settings.h"
#include "TimeUtils.h"

#include <QDir>
#include <QTimeZone>

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
        return Qt::Checked;
    if (key == QLatin1String(GC_GARMIN_HWMARK))
        return 25;
    return def;
}

QVariant GSettings::cvalue(QString, QString, QVariant def)
{
    return def;
}

static GSettings testSettings(
    QStringLiteral("GoldenCheetah"),
    QStringLiteral("FitReaderIntegrityTest"));
GSettings *appsettings = &testSettings;
int OperatingSystem = LINUX;
QString gcroot = QDir::tempPath();

DataProcessorFactory *DataProcessorFactory::instance_ = nullptr;
bool DataProcessorFactory::autoprocess = false;

DataProcessorFactory &DataProcessorFactory::instance()
{
    static DataProcessorFactory factory;
    return factory;
}

DataProcessorFactory::~DataProcessorFactory()
{
}

QMap<QString, DataProcessor*> DataProcessorFactory::getProcessors(bool) const
{
    return {};
}

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
