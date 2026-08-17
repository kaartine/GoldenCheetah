/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_BluetoothTrainerCapabilities_h
#define _GC_BluetoothTrainerCapabilities_h

#include "WorkoutRideTargetPlanner.h"

enum class BluetoothTrainerControlProtocol
{
    None,
    TacxUart,
    WahooKickr,
    KurtInRide,
    KurtSmartControl,
    Ftms
};

struct BluetoothFtmsControlFeatures
{
    bool targetPower = false;
    bool targetResistance = false;
    bool simulation = false;
};

class BluetoothTrainerCapabilities
{
public:
    static TrainerControlCapabilities forProtocol(
            BluetoothTrainerControlProtocol protocol,
            const BluetoothFtmsControlFeatures &ftms = {});
};

#endif
