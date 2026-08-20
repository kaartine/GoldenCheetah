/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameMesh_h
#define _GC_WorkoutGameMesh_h

#include "WorkoutGameRoadProjection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

enum class WorkoutGameMeshMaterial
{
    Dirt,
    DirtEdge,
    Bypass,
    WoodSide,
    WoodTop,
    RockSide,
    RockTop,
    DropFace
};

struct WorkoutGameMeshVertex
{
    double forwardMeters = 0.0;
    double rightMeters = 0.0;
    double upMeters = 0.0;
    double u = 0.0;
    double v = 0.0;
};

struct WorkoutGameMeshTriangle
{
    std::array<std::uint32_t, 3> indices = {0u, 0u, 0u};
    WorkoutGameMeshMaterial material = WorkoutGameMeshMaterial::Dirt;
};

struct WorkoutGameCollisionBox
{
    double forwardMeters = 0.0;
    double rightMeters = 0.0;
    double upMeters = 0.0;
    double halfForwardMeters = 0.0;
    double halfRightMeters = 0.0;
    double halfUpMeters = 0.0;
};

struct WorkoutGameMeshConnector
{
    double forwardMeters = 0.0;
    double halfWidthMeters = 0.0;
    double elevationMeters = 0.0;
};

struct WorkoutGameMesh
{
    bool ready = false;
    double lengthMeters = 0.0;
    WorkoutGameMeshConnector entry;
    WorkoutGameMeshConnector exit;
    std::vector<WorkoutGameMeshVertex> vertices;
    std::vector<WorkoutGameMeshTriangle> triangles;
    std::vector<WorkoutGameCollisionBox> colliders;
};

struct WorkoutGameMeshInstance
{
    WorkoutGameMesh mesh;
    double anchorDistanceMeters = 0.0;
    double lateralMeters = 0.0;
    double elevationMeters = 0.0;
    double yawDegrees = 0.0;
    double forwardScale = 1.0;
    double rightScale = 1.0;
    double upScale = 1.0;
    bool anchorToBaseSurface = false;
};

struct WorkoutGameProjectedMeshVertex
{
    double x = 0.0;
    double y = 0.0;
    double depthMeters = 0.0;
    double u = 0.0;
    double v = 0.0;
};

struct WorkoutGameProjectedMeshTriangle
{
    std::array<WorkoutGameProjectedMeshVertex, 3> vertices;
    WorkoutGameMeshMaterial material = WorkoutGameMeshMaterial::Dirt;
    double depthMeters = 0.0;
};

class WorkoutGameMeshLibrary
{
public:
    static WorkoutGameMesh feature(
            WorkoutGameTerrainKind terrain,
            double difficulty);
    static WorkoutGameMesh trailTile(
            double lengthMeters,
            double entryHalfWidthMeters,
            double exitHalfWidthMeters,
            double riseMeters);
    static WorkoutGameMesh bypassRibbon(
            double lengthMeters,
            double lateralMeters,
            double halfWidthMeters);
    static bool valid(const WorkoutGameMesh &mesh);
};

class WorkoutGameMeshProjector
{
public:
    static std::vector<WorkoutGameProjectedMeshTriangle> project(
            const WorkoutGameMeshInstance &instance,
            const WorkoutGameRoadProjectionFrame &road);
};

#endif
