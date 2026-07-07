/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_TrainingDeviceSelection_h
#define _GC_TrainingDeviceSelection_h

#include "BluetoothDeviceTypes.h"

#include <QList>

namespace TrainingDeviceSelection
{

inline bool isHeartRateDevice(int type)
{
    return type == DEV_BT40_HEARTRATE;
}

struct Selection {
    QList<int> active;
    int heartRateSource = -1;
};

inline Selection select(const QList<int> &selected, const QList<int> &deviceTypes)
{
    Selection result;
    int selectedHeartRate = -1;
    bool hasValidSelection = false;

    foreach (int index, selected) {
        if (index < 0 || index >= deviceTypes.size() ||
            result.active.contains(index) || index == selectedHeartRate) {
            continue;
        }

        hasValidSelection = true;
        if (isHeartRateDevice(deviceTypes.at(index))) {
            if (selectedHeartRate < 0) selectedHeartRate = index;
        } else {
            result.active.append(index);
        }
    }

    if (!hasValidSelection) return result;

    if (selectedHeartRate < 0) {
        for (int index = 0; index < deviceTypes.size(); ++index) {
            if (isHeartRateDevice(deviceTypes.at(index))) {
                selectedHeartRate = index;
                break;
            }
        }
    }

    if (selectedHeartRate >= 0) {
        result.active.append(selectedHeartRate);
        result.heartRateSource = selectedHeartRate;
    }

    return result;
}

inline QList<int> withHeartRateCompanions(const QList<int> &selected,
                                          const QList<int> &deviceTypes)
{
    return select(selected, deviceTypes).active;
}

inline int heartRateSource(const QList<int> &active, const QList<int> &deviceTypes)
{
    foreach (int index, active) {
        if (index >= 0 && index < deviceTypes.size() &&
            isHeartRateDevice(deviceTypes.at(index))) {
            return index;
        }
    }

    return -1;
}

inline bool routesHeartRate(int source, int device)
{
    return source >= 0 && source == device;
}

}

#endif
