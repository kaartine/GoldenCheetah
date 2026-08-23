/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRoadProjection_h
#define _GC_WorkoutGameRoadProjection_h

#include "WorkoutGameRoadCourse.h"

#include <cstddef>
#include <limits>
#include <vector>

struct WorkoutGameRoadProjectionConfig
{
    double viewportWidth = 1280.0;
    double viewportHeight = 720.0;
    double horizonRatio = 0.34;
    double cameraHeightMeters = 1.55;
    double cameraElevationMeters = std::numeric_limits<double>::quiet_NaN();
    double verticalExaggeration = 1.35;
    double fieldOfViewDegrees = 62.0;
    double nearDistanceMeters = 1.4;
    double visibleDistanceMeters = 105.0;
    int sliceCount = 96;
};

struct WorkoutGameRoadProjectedSlice
{
    double worldDistanceMeters = 0.0;
    double depthMeters = 0.0;
    double centerX = 0.0;
    double centerY = 0.0;
    double halfWidthPixels = 0.0;
    double halfWidthMeters = 0.0;
    double pixelsPerMeter = 0.0;
    double surfaceOffsetMeters = 0.0;
    double surfaceElevationMeters = 0.0;
    double gradePercent = 0.0;
    double occlusionY = 0.0;
    std::size_t pieceIndex = 0;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
};

struct WorkoutGameRoadProjectedPoint
{
    bool ready = false;
    double x = 0.0;
    double y = 0.0;
    double depthMeters = 0.0;
    double occlusionY = 0.0;
    bool visible = false;
};

struct WorkoutGameRoadProjectionFrame
{
    bool ready = false;
    double riderScreenX = 0.0;
    double riderScreenY = 0.0;
    double renderedGradePercent = 0.0;
    double visibleElevationChangeMeters = 0.0;
    double verticalExaggeration = 1.0;
    std::vector<WorkoutGameRoadProjectedSlice> slices;
};

class WorkoutGameRoadProjection
{
public:
    static WorkoutGameRoadProjectionFrame project(
            const WorkoutGameRoadCourse &course,
            double riderDistanceMeters,
            const WorkoutGameRoadProjectionConfig &config = {});
    static WorkoutGameRoadProjectedPoint projectPoint(
            const WorkoutGameRoadProjectionFrame &frame,
            double worldDistanceMeters,
            double lateralMeters,
            double elevationMeters,
            bool relativeToBaseSurface = false);
};

#endif
