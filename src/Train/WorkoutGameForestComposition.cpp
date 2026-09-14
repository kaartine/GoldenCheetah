/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameForestComposition.h"

#include <algorithm>
#include <cmath>

namespace {

std::uint32_t mix(std::uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

double unitByte(std::uint32_t value, int shift)
{
    return double((value >> shift) & 255u) / 255.0;
}

}

WorkoutGameForestBiome WorkoutGameForestComposition::biomeAt(
        std::uint32_t courseSeed, double distanceMeters)
{
    const int zone = std::max(0, int(std::floor(
            std::max(0.0, distanceMeters) / BiomeLengthMeters)));
    const std::uint32_t random = mix(
            courseSeed ^ std::uint32_t((zone + 1) * 0x9e3779b9u));
    return static_cast<WorkoutGameForestBiome>(random % 3u);
}

WorkoutGameForestSlotPlan WorkoutGameForestComposition::plan(
        std::uint32_t courseSeed, int slot, double distanceMeters)
{
    const std::uint32_t random = mix(
            courseSeed ^ std::uint32_t((slot + 1) * 0x27d4eb2du));
    WorkoutGameForestSlotPlan result;
    result.biome = biomeAt(courseSeed, distanceMeters);
    result.clusterSide = (random & 1u) == 0u ? -1 : 1;

    static constexpr int ClusterVariants[3][3] = {
        {2, 3, 5}, // heather, deadwood and saplings on dry pine ground
        {0, 1, 4}, // bilberry, fern and shrubs in moist spruce forest
        {0, 3, 1}  // granite-led groups in rocky mixed forest
    };
    static constexpr int FloorVariants[3][6] = {
        {4, 7, 10, 13, 14, 6},
        {3, 5, 6, 9, 12, 15},
        {0, 1, 2, 3, 8, 13}
    };
    const int biomeIndex = static_cast<int>(result.biome);
    result.clusterVariant = ClusterVariants[biomeIndex][(random >> 5) % 3u];
    result.floorVariant = FloorVariants[biomeIndex][(random >> 12) % 6u];
    result.clusterScale = 1.15 + 0.45 * unitByte(random, 16);
    result.floorScale = 0.95 + 0.45 * unitByte(random, 8);
    result.clusterEdgeOffsetMeters = 0.16 + 0.34 * unitByte(random, 24);
    result.floorEdgeOffsetMeters = 1.75 + 0.90 * unitByte(random, 20);
    result.clusterYawOffsetDegrees = -22.0 + 44.0 * unitByte(random, 8);
    result.floorYawOffsetDegrees = -25.0 + 50.0 * unitByte(random, 16);
    return result;
}
