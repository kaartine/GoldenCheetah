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
#include "Specification.h"
#include "SplineLookup.h"
#include "Units.h"
#include "WPrime.h"
#include "Zones.h"

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

int rideFileTestFilterHrvCalls = 0;
double rideFileTestFilterHrvMinimum = 0.0;
double rideFileTestFilterHrvMaximum = 0.0;
double rideFileTestFilterHrvRelative = 0.0;
int rideFileTestFilterHrvWindow = 0;

void FilterHrv(
    XDataSeries *,
    double minimum,
    double maximum,
    double relative,
    int window)
{
    ++rideFileTestFilterHrvCalls;
    rideFileTestFilterHrvMinimum = minimum;
    rideFileTestFilterHrvMaximum = maximum;
    rideFileTestFilterHrvRelative = relative;
    rideFileTestFilterHrvWindow = window;
}

double Specification::secsStart() const
{
    return -1.0;
}

double Specification::secsEnd() const
{
    return -1.0;
}

double Athlete::getWeight(QDate, RideFile *)
{
    return 75.0;
}

double Athlete::getHeight(RideFile *)
{
    return 1.75;
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

int Zones::getCP(int) const
{
    return 0;
}
