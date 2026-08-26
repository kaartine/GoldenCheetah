/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "TrainingDataGeneratorTargetRouting.h"

#include "RealtimeData.h"

bool TrainingDataGeneratorTargetRouting::publishPowerSourceState(
        int deviceType,
        const RealtimeData &source,
        RealtimeData &destination)
{
    if (!acceptsDeviceType(deviceType)) return false;
    destination.setLoad(source.getLoad());
    destination.setName(source.getName());
    return true;
}
