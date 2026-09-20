/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_RIDEFILEPOSTPROCESSINPUTS_H
#define GC_RIDEFILEPOSTPROCESSINPUTS_H

#include "RideFileDerivedSeriesInputs.h"

#include <QString>
#include <QStringList>

struct RideFileLegacyNotes
{
    bool readable = false;
    QString text;
};

struct RideFileHrvFilterInputs
{
    double maximum = 2000.0;
    double minimum = 270.0;
    double relativeFilter = 0.2;
    int window = 20;
};

struct RideFilePostProcessInputs
{
    QStringList orderedIntervalMetadataNames;
    RideFileLegacyNotes notes;
    bool athleteTagAvailable = false;
    QString athleteName;
    RideFileHrvFilterInputs hrv;
    bool recalculateDerivedSeries = false;
    RideFileDerivedSeriesInputs derivedSeries;
};

#endif // GC_RIDEFILEPOSTPROCESSINPUTS_H
