/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_RIDEFILEDERIVEDSERIESINPUTS_H
#define GC_RIDEFILEDERIVEDSERIESINPUTS_H

struct RideFileDerivedSeriesInputs
{
    bool powerZonesAvailable = false;
    int configuredCp = 0;
    int configuredWheelSizeMillimeters = 2100;
};

#endif // GC_RIDEFILEDERIVEDSERIESINPUTS_H
