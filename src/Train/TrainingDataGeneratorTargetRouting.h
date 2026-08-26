/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_TrainingDataGeneratorTargetRouting_h
#define _GC_TrainingDataGeneratorTargetRouting_h

#include "DeviceTypes.h"

#include <algorithm>
#include <cmath>

class RealtimeData;

namespace TrainingDataGeneratorTargetRouting {

inline bool acceptsDeviceType(int deviceType)
{
    return deviceType == DEV_NULL;
}

inline bool normalizeTarget(double requestedWatts, double &normalizedWatts)
{
    if (!std::isfinite(requestedWatts) || requestedWatts < 0.0) return false;
    normalizedWatts = std::clamp(requestedWatts, 0.0, 2500.0);
    return true;
}

bool publishPowerSourceState(
        int deviceType,
        const RealtimeData &source,
        RealtimeData &destination);

}

#endif
