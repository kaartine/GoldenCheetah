/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_BluetoothDeviceTypes_h
#define _GC_BluetoothDeviceTypes_h

#include <vector>

#define DEV_BT40           0x2000
#define DEV_BT40_HEARTRATE 0x2001

namespace BluetoothDeviceTypes
{

enum class DeviceRole {
    Trainer,
    HeartRateOnly
};

enum class ServiceKind {
    HeartRate,
    TrainerControl,
    OtherTelemetry
};

enum class LinkState {
    Unconnected,
    Active,
    Closing
};

inline DeviceRole roleForType(int type)
{
    return type == DEV_BT40_HEARTRATE
            ? DeviceRole::HeartRateOnly
            : DeviceRole::Trainer;
}

inline bool permitsEmptyProfile(int type)
{
    return roleForType(type) != DeviceRole::HeartRateOnly;
}

inline bool acceptsDiscoveredDevice(int type, bool advertisesHeartRate)
{
    return roleForType(type) != DeviceRole::HeartRateOnly || advertisesHeartRate;
}

inline bool acceptsService(DeviceRole role, ServiceKind service)
{
    return role != DeviceRole::HeartRateOnly || service == ServiceKind::HeartRate;
}

inline bool allowsTrainerControl(DeviceRole role)
{
    return role != DeviceRole::HeartRateOnly;
}

template<typename IndexRange>
bool containsDevice(const IndexRange &devices, int candidate)
{
    for (int device : devices) {
        if (device == candidate) return true;
    }
    return false;
}

template<typename IndexRange>
int resolveHeartRateSource(int requestedSource, int automaticSource,
                           bool preserveRequestedSource,
                           const IndexRange &activeDevices)
{
    if (preserveRequestedSource) {
        if (requestedSource < 0) return -1;
        return containsDevice(activeDevices, requestedSource)
                ? requestedSource
                : -1;
    }

    if (automaticSource >= 0 &&
        containsDevice(activeDevices, automaticSource)) {
        return automaticSource;
    }

    return requestedSource >= 0 &&
            containsDevice(activeDevices, requestedSource)
            ? requestedSource
            : -1;
}

struct ControllerStartResult {
    ControllerStartResult(bool success = true, int failedDevice = -1,
                          int errorCode = 0) :
        success(success), failedDevice(failedDevice), errorCode(errorCode)
    {
    }

    bool success;
    int failedDevice;
    int errorCode;
};

template<typename IndexRange, typename StartController, typename StopController>
ControllerStartResult startControllers(const IndexRange &activeDevices,
                                       StartController startController,
                                       StopController stopController)
{
    std::vector<int> startedDevices;

    for (int device : activeDevices) {
        const int errorCode = startController(device);
        if (errorCode == 0) {
            startedDevices.push_back(device);
            continue;
        }

        stopController(device);
        for (auto started = startedDevices.rbegin();
             started != startedDevices.rend(); ++started) {
            stopController(*started);
        }
        return {false, device, errorCode};
    }

    return {};
}

inline bool shouldConnect(bool requested, LinkState state)
{
    return requested && state == LinkState::Unconnected;
}

inline bool shouldDisconnect(LinkState state)
{
    return state == LinkState::Active;
}

inline bool shouldReconnect(bool requested, LinkState state)
{
    return shouldConnect(requested, state);
}

}

#endif
