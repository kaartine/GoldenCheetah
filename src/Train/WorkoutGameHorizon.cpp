/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameHorizon.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double Pi = 3.14159265358979323846;

double ridge(
        double x,
        double phase,
        double base,
        double primary,
        double secondary)
{
    return std::clamp(
            base
                + primary * std::sin(2.0 * Pi * (x * 1.15 + phase))
                + secondary * std::sin(
                    2.0 * Pi * (x * 2.7 - phase * 0.43)),
            0.18, 0.48);
}

}

WorkoutGameHorizonSnapshot WorkoutGameHorizon::build(
        std::uint32_t seed,
        double riderDistanceMeters,
        int requestedSampleCount)
{
    WorkoutGameHorizonSnapshot result;
    if (!std::isfinite(riderDistanceMeters)) return result;
    const int sampleCount = std::clamp(requestedSampleCount, 8, 128);
    result.farRidgeY.reserve(std::size_t(sampleCount + 1));
    result.nearRidgeY.reserve(std::size_t(sampleCount + 1));
    const double seedPhase = double(seed % 8191u) / 8191.0;
    const double scroll = riderDistanceMeters / 900.0;
    for (int index = 0; index <= sampleCount; ++index) {
        const double x = double(index) / double(sampleCount);
        result.farRidgeY.push_back(ridge(
                x, seedPhase + scroll * 0.28, 0.310, 0.055, 0.024));
        result.nearRidgeY.push_back(ridge(
                x, seedPhase * 0.61 + scroll * 0.52,
                0.365, 0.085, 0.038));
    }
    result.ready = true;
    return result;
}
