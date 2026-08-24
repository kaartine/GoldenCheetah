/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameTrailTile_h
#define _GC_WorkoutGameTrailTile_h

#include "WorkoutGameMesh.h"

#include <cstddef>
#include <vector>

struct WorkoutGameTrailTile
{
    bool ready = false;
    double entryDistanceMeters = 0.0;
    double exitDistanceMeters = 0.0;
    double entryHalfWidthMeters = 0.0;
    double exitHalfWidthMeters = 0.0;
    std::size_t sourceSectionIndex = 0;
    std::vector<WorkoutGameMeshInstance> mainLine;
    WorkoutGameMeshInstance bypass;
};

class WorkoutGameTrailTileAssembler
{
public:
    static WorkoutGameTrailTile challenge(
            const WorkoutGameRoadCourse &course,
            const WorkoutGameRoadPiece &piece);
};

#endif
