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

#ifndef GC_MERGEACTIVITYTIMEOFFSET_H
#define GC_MERGEACTIVITYTIMEOFFSET_H

namespace MergeActivityTimeOffset {

double sampleOffsetSeconds(
    int sampleOffset, double recordingIntervalSeconds) noexcept;
double shiftTimestamp(
    double timestampSeconds,
    int sampleOffset,
    double recordingIntervalSeconds) noexcept;
double displayAdjustmentSeconds(
    int sliderOffset, double recordingIntervalSeconds) noexcept;

} // namespace MergeActivityTimeOffset

#endif // GC_MERGEACTIVITYTIMEOFFSET_H
