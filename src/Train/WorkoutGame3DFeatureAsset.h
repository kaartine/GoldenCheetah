/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGame3DFeatureAsset_h
#define _GC_WorkoutGame3DFeatureAsset_h

#include "WorkoutGameRoadCourse.h"

struct WorkoutGame3DFeatureAssetSnapshot
{
    bool ready = false;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    double xMeters = 0.0;
    double yMeters = 0.0;
    double zMeters = 0.0;
    double yawDegrees = 0.0;
    double pitchDegrees = 0.0;
    double scaleY = 1.0;
    double scaleZ = 1.0;
};

class WorkoutGame3DFeatureAsset
{
public:
    static WorkoutGame3DFeatureAssetSnapshot place(
            const WorkoutGameRoadCourse &course,
            const WorkoutGameRoadPiece &piece);
};

#endif
