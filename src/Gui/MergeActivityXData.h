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

#ifndef GC_MERGEACTIVITYXDATA_H
#define GC_MERGEACTIVITYXDATA_H

#include <memory>

class XDataSeries;

namespace MergeActivityXData {

// Deep-copy points shifted onto the inclusive merged activity timeline.
std::unique_ptr<XDataSeries> shiftedCopy(
    const XDataSeries &source,
    int sampleOffset,
    double recordingIntervalSeconds,
    double timelineStartSeconds,
    double timelineStopSeconds);

} // namespace MergeActivityXData

#endif // GC_MERGEACTIVITYXDATA_H
