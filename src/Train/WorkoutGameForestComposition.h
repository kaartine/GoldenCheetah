/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameForestComposition_h
#define _GC_WorkoutGameForestComposition_h

#include <cstdint>

enum class WorkoutGameForestBiome
{
    DryPine = 0,
    MoistSpruce,
    RockyMixed
};

struct WorkoutGameForestSlotPlan
{
    WorkoutGameForestBiome biome = WorkoutGameForestBiome::DryPine;
    int clusterSide = -1;
    int clusterVariant = 0;
    int floorVariant = 0;
    double clusterScale = 1.0;
    double floorScale = 1.0;
    double clusterEdgeOffsetMeters = 0.14;
    double floorEdgeOffsetMeters = 1.7;
    double clusterYawOffsetDegrees = 0.0;
    double floorYawOffsetDegrees = 0.0;
};

class WorkoutGameForestComposition
{
public:
    static constexpr double BiomeLengthMeters = 45.0;

    static WorkoutGameForestBiome biomeAt(
            std::uint32_t courseSeed, double distanceMeters);
    static WorkoutGameForestSlotPlan plan(
            std::uint32_t courseSeed, int slot, double distanceMeters);
};

#endif
