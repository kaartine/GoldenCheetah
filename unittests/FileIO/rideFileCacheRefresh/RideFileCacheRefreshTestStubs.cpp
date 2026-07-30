/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "Athlete.h"
#include "HrZones.h"
#include "PaceZones.h"
#include "WPrime.h"
#include "Zones.h"

#include <QString>

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

int Zones::getWprime(int) const
{
    return 0;
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
