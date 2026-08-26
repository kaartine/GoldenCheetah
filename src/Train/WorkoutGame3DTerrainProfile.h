/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGame3DTerrainProfile_h
#define _GC_WorkoutGame3DTerrainProfile_h

#include "WorkoutGameRoadCourse.h"

#include <array>
#include <cstddef>
#include <cstdint>

struct WorkoutGame3DTerrainProfileVertex
{
    double lateralMeters = 0.0;
    double elevationMeters = 0.0;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
};

struct WorkoutGame3DTerrainProfileSnapshot
{
    static constexpr std::size_t VertexCount = 8;

    bool ready = false;
    std::array<WorkoutGame3DTerrainProfileVertex, VertexCount> vertices;
};

class WorkoutGame3DTerrainProfile
{
public:
    static WorkoutGame3DTerrainProfileSnapshot build(
            const WorkoutGameRoadSample &road,
            double distanceMeters,
            std::uint32_t seed);
    static double bypassSurfaceElevationMeters(
            const WorkoutGameRoadSample &road,
            double distanceMeters,
            std::uint32_t seed,
            double lateralMeters,
            double treadLiftMeters);
    static double elevationAtLateral(
            const WorkoutGame3DTerrainProfileSnapshot &profile,
            double lateralMeters);
};

#endif
