/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_TrainingDeviceWizardRouting_h
#define _GC_TrainingDeviceWizardRouting_h

#include "DeviceTypes.h"

namespace TrainingDeviceWizardRouting {

constexpr int ScanPage = 20;
constexpr int FortiusFirmwarePage = 30;
constexpr int ImagicFirmwarePage = 35;
constexpr int AntPairingPage = 50;
constexpr int BluetoothPairingPage = 55;
constexpr int VirtualPowerPage = 60;
constexpr int ConfirmationPage = 70;

inline int pageAfterSuccessfulScan(int deviceType)
{
    switch (deviceType) {
    case DEV_BT40:
    case DEV_BT40_HEARTRATE:
        return BluetoothPairingPage;
    case DEV_ANTLOCAL:
        return AntPairingPage;
    case DEV_NULL:
        return ConfirmationPage;
    case DEV_FORTIUS:
        return FortiusFirmwarePage;
    case DEV_IMAGIC:
        return ImagicFirmwarePage;
    default:
        return VirtualPowerPage;
    }
}

}

#endif // _GC_TrainingDeviceWizardRouting_h
