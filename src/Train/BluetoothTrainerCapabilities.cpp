/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "BluetoothTrainerCapabilities.h"

TrainerControlCapabilities BluetoothTrainerCapabilities::forProtocol(
        BluetoothTrainerControlProtocol protocol,
        const BluetoothFtmsControlFeatures &ftms)
{
    TrainerControlCapabilities result;

    switch (protocol) {
    case BluetoothTrainerControlProtocol::TacxUart:
        result.targetPower = true;
        result.simulation = true;
        break;
    case BluetoothTrainerControlProtocol::WahooKickr:
    case BluetoothTrainerControlProtocol::KurtSmartControl:
        result.targetPower = true;
        result.targetResistance = true;
        result.simulation = true;
        break;
    case BluetoothTrainerControlProtocol::Ftms:
        result.targetPower = ftms.targetPower;
        result.targetResistance = ftms.targetResistance;
        result.simulation = ftms.simulation;
        break;
    case BluetoothTrainerControlProtocol::None:
    case BluetoothTrainerControlProtocol::KurtInRide:
        break;
    }

    return result;
}
