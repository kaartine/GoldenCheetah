/*
 * Copyright (c) 2026 GoldenCheetah contributors
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
#include "HrZones.h"
#include "PaceZones.h"
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

GSettings::~GSettings()
{
}

QVariant
GSettings::value(
    const QObject *,
    const QString,
    const QVariant defaultValue)
{
    return defaultValue;
}

QVariant
GSettings::cvalue(
    QString,
    QString,
    QVariant defaultValue)
{
    return defaultValue;
}

static GSettings testSettings(
    QStringLiteral("GoldenCheetah"),
    QStringLiteral("RideFileCacheRefreshTest"));
GSettings *appsettings = &testSettings;

GlobalContext *
GlobalContext::context()
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

void Context::reportCacheWriteFailure(
    const QString &,
    const QString &)
{
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

double Athlete::getWeight(QDate, RideFile *)
{
    return 75.0;
}

double Athlete::getHeight(RideFile *)
{
    return 1.75;
}

double Specification::secsStart() const
{
    return -1.0;
}

double Specification::secsEnd() const
{
    return -1.0;
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

QColor GCColor::getColor(int)
{
    return {};
}

QString kphToPace(double, bool, bool)
{
    return {};
}

int Zones::whichRange(const QDate &) const
{
    return -1;
}

int Zones::whichZone(int, double) const
{
    return -1;
}

int Zones::getCP(int) const
{
    return 0;
}

int Zones::getAeT(int) const
{
    return 0;
}

int Zones::getFTP(int) const
{
    return 0;
}

int Zones::getWprime(int) const
{
    return 0;
}

int Zones::getPmax(int) const
{
    return 0;
}

QList<int> Zones::getZoneLows(int) const
{
    return {};
}

QList<int> Zones::getZoneHighs(int) const
{
    return {};
}

int HrZones::whichRange(const QDate &) const
{
    return -1;
}

int HrZones::whichZone(int, double) const
{
    return -1;
}

int HrZones::getLT(int) const
{
    return 0;
}

int HrZones::getAeT(int) const
{
    return 0;
}

int HrZones::getRestHr(int) const
{
    return 0;
}

int HrZones::getMaxHr(int) const
{
    return 0;
}

QList<int> HrZones::getZoneLows(int) const
{
    return {};
}

QList<int> HrZones::getZoneHighs(int) const
{
    return {};
}

int PaceZones::whichRange(const QDate &) const
{
    return -1;
}

int PaceZones::whichZone(int, double) const
{
    return -1;
}

double PaceZones::getCV(int) const
{
    return 0.0;
}

double PaceZones::getAeT(int) const
{
    return 0.0;
}

QList<double> PaceZones::getZoneLows(int) const
{
    return {};
}

QList<double> PaceZones::getZoneHighs(int) const
{
    return {};
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

void WPrime::check()
{
}
