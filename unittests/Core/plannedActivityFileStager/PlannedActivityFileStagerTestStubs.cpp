/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "Athlete.h"
#include "Colors.h"
#include "CompressedActivityFile.h"
#include "Context.h"
#include "FilterHRV.h"
#include "Settings.h"
#include "Specification.h"
#include "SplineLookup.h"
#include "Units.h"
#include "WPrime.h"
#include "Zones.h"

#include <QString>

GSettings::GSettings(QString, QString)
    : newFormat(false)
{
}

GSettings::~GSettings() = default;

QVariant GSettings::value(
    const QObject *,
    const QString,
    const QVariant defaultValue)
{
    return defaultValue;
}

QVariant GSettings::cvalue(
    QString,
    QString,
    QVariant defaultValue)
{
    return defaultValue;
}

static GSettings testSettings(
    QStringLiteral("GoldenCheetah"),
    QStringLiteral("PlannedActivityFileStagerTest"));
GSettings *appsettings = &testSettings;

GlobalContext *GlobalContext::context()
{
    return nullptr;
}

namespace CompressedActivityFile {

bool extractSingleFile(
    std::unique_ptr<QIODevice>,
    Format,
    QIODevice *)
{
    return false;
}

} // namespace CompressedActivityFile

void FilterHrv(
    XDataSeries *,
    double,
    double,
    double,
    int)
{
}

QString kphToPace(double, bool, bool)
{
    return {};
}

void SplineLookup::update(
    const QwtSplineBasis &,
    const QPolygonF &,
    double)
{
}

double SplineLookup::valueY(double) const
{
    return 0.0;
}

double Athlete::getWeight(QDate, RideFile *)
{
    return 75.0;
}

double Athlete::getHeight(RideFile *)
{
    return 1.75;
}

QColor GCColor::getColor(int)
{
    return {};
}

double Specification::secsStart() const
{
    return -1.0;
}

double Specification::secsEnd() const
{
    return -1.0;
}

int Zones::whichRange(const QDate &) const
{
    return -1;
}

int Zones::getCP(int) const
{
    return 0;
}

WPrime::WPrime()
    : minY(0.0)
    , maxY(0.0)
    , TAU(0.0)
    , PCP_(0.0)
    , CP(0.0)
    , WPRIME(0.0)
    , EXP(0.0)
    , rideFile(nullptr)
    , last(0)
    , wasIntegral(false)
{
}

void WPrime::setRide(RideFile *ride)
{
    rideFile = ride;
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
