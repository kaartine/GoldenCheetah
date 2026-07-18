/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#include "MergeActivityRidePreparation.h"

#include "RideFile.h"

#include <memory>

namespace MergeActivityRidePreparation {

bool
replaceWorkingRide(
    RideFile *&working, RideFile *source, double recordingIntervalSeconds)
{
    if (!source) {
        RideFile *previous = working;
        working = nullptr;
        delete previous;
        return false;
    }

    std::unique_ptr<RideFile> replacement(
        source->resample(recordingIntervalSeconds));
    if (!replacement) return false;

    for (XDataSeries *xdata : source->xdata()) {
        if (xdata) {
            replacement->addXData(
                xdata->name, new XDataSeries(*xdata));
        }
    }

    RideFile *previous = working;
    working = replacement.release();
    delete previous;
    return true;
}

} // namespace MergeActivityRidePreparation
