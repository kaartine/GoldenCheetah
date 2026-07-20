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

#include "MergeActivityXData.h"

#include "MergeActivityTimeOffset.h"
#include "RideFile.h"

namespace MergeActivityXData {

std::unique_ptr<XDataSeries> shiftedCopy(
    const XDataSeries &source,
    int sampleOffset,
    double recordingIntervalSeconds,
    double timelineStartSeconds,
    double timelineStopSeconds)
{
    auto shifted = std::make_unique<XDataSeries>();
    shifted->name = source.name;
    shifted->valuename = source.valuename;
    shifted->unitname = source.unitname;
    shifted->valuetype = source.valuetype;

    if (timelineStopSeconds < timelineStartSeconds) return shifted;

    shifted->datapoints.reserve(source.datapoints.size());
    for (const XDataPoint *sourcePoint : source.datapoints) {
        if (!sourcePoint) continue;

        const double shiftedSeconds =
            MergeActivityTimeOffset::shiftTimestamp(
                sourcePoint->secs,
                sampleOffset,
                recordingIntervalSeconds);
        if (!(shiftedSeconds >= timelineStartSeconds
              && shiftedSeconds <= timelineStopSeconds)) {
            continue;
        }

        auto point = std::make_unique<XDataPoint>(*sourcePoint);
        point->secs = shiftedSeconds;
        shifted->datapoints.append(point.get());
        point.release();
    }

    return shifted;
}

} // namespace MergeActivityXData
