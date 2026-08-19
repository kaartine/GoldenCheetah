/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameTrailScene_h
#define _GC_WorkoutGameTrailScene_h

#include "WorkoutGameWorld.h"
#include "WorkoutGameFeatureCatalog.h"

#include <cstdint>
#include <vector>

struct WorkoutGameTrailPoint
{
    double worldDistanceMeters = 0.0;
    double xNormalized = 0.0;
    double centerYNormalized = 0.0;
    double farEdgeYNormalized = 0.0;
    double nearEdgeYNormalized = 0.0;
};

struct WorkoutGameTrailProp
{
    WorkoutGameTrailPropKind kind = WorkoutGameTrailPropKind::Pebble;
    std::uint32_t variant = 0;
    double worldDistanceMeters = 0.0;
    double lateralPosition = 0.0;
    double xNormalized = 0.0;
    double yNormalized = 0.0;
    double farEdgeYNormalized = 0.0;
    double nearEdgeYNormalized = 0.0;
    double scale = 1.0;
    double depthKey = 0.0;
};

struct WorkoutGameTrailSceneSnapshot
{
    bool ready = false;
    double riderXNormalized = 0.28;
    double riderYNormalized = 0.61;
    std::vector<WorkoutGameTrailPoint> points;
    std::vector<WorkoutGameTrailProp> props;
};

class WorkoutGameTrailScene
{
public:
    static constexpr double VisibleMeters = 42.0;
    static constexpr double RiderXNormalized = 0.28;

    static WorkoutGameTrailSceneSnapshot build(
            const WorkoutGameWorldSnapshot &world,
            int sampleCount = 72);
};

#endif
