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

    const int zone = std::max(0, int(std::floor(
            std::max(0.0, distanceMeters) / BiomeLengthMeters)));
    const int palette = (zone / 4) % 5;
    // Each 180 m chapter rotates the catalog while neighboring biomes share
    // anchor variants. A resident window therefore stays at nine or fewer
    // GPU batches without making a long ride visually repetitive.
    static constexpr int ClusterVariants[3][5][2] = {
        {{0, 2}, {0, 3}, {0, 5}, {0, 2}, {0, 3}},
        {{0, 1}, {0, 4}, {0, 1}, {0, 4}, {0, 1}},
        {{0, 3}, {0, 1}, {0, 3}, {0, 1}, {0, 3}}
    };
    static constexpr int FloorVariants[3][5][3] = {
        {{3, 6, 4}, {3, 6, 7}, {3, 6, 10}, {3, 6, 14}, {3, 6, 13}},
        {{3, 6, 5}, {3, 6, 9}, {3, 6, 12}, {3, 6, 15}, {3, 6, 11}},
        {{3, 6, 0}, {3, 6, 1}, {3, 6, 2}, {3, 6, 8}, {3, 6, 13}}
    };
    const int biomeIndex = static_cast<int>(result.biome);
    result.clusterVariant =
            ClusterVariants[biomeIndex][palette][(random >> 5) % 2u];
    result.floorVariant =
            FloorVariants[biomeIndex][palette][(random >> 12) % 3u];
    result.clusterScale = 1.15 + 0.45 * unitByte(random, 16);
    result.floorScale = 0.95 + 0.45 * unitByte(random, 8);
    result.clusterEdgeOffsetMeters = 0.16 + 0.34 * unitByte(random, 24);
    result.floorEdgeOffsetMeters = 1.75 + 0.90 * unitByte(random, 20);
    result.clusterYawOffsetDegrees = -22.0 + 44.0 * unitByte(random, 8);
    result.floorYawOffsetDegrees = -25.0 + 50.0 * unitByte(random, 16);
    return result;
}
