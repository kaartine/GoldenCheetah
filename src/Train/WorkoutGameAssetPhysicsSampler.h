/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameAssetPhysicsSampler_h
#define _GC_WorkoutGameAssetPhysicsSampler_h

#include "WorkoutGameAssetPhysicsSnapshot.h"

#include <cstddef>
#include <vector>

struct WorkoutGameAssetPhysicsSample
{
    bool bound = false;
    bool surfacePresent = false;
    bool obstacleContact = false;
    bool materialDefined = false;
    WorkoutGameAssetPhysicsOperation operation =
            WorkoutGameAssetPhysicsOperation::AddObstacle;
    double offsetMeters = 0.0;
    double coulombFriction = 0.0;
    double restitution = 0.0;
};

enum class WorkoutGameAssetRenderFitStatus
{
    Unbound,
    Ready,
    Invalid
};

struct WorkoutGameAssetRenderFit
{
    WorkoutGameAssetRenderFitStatus status =
            WorkoutGameAssetRenderFitStatus::Unbound;
    QString assetId;
    QString variantKey;
    std::int32_t obstacleAnchorMm = 0;
    std::int16_t obstacleAnchorMicrometerRemainder = 0;
    std::int32_t nativeForwardOriginMm = 0;
    std::uint32_t nativeForwardExtentMm = 0;
    std::uint32_t nativeUpExtentMm = 0;
    std::uint32_t resolvedExtentMm = 0;
    bool exactLegacy = false;
    double exactObstacleAnchorMeters = 0.0;
    double exactStartMeters = 0.0;
    double exactEndMeters = 0.0;
    double exactHeightMeters = 0.0;
};

struct WorkoutGameAssetRenderTransform
{
    WorkoutGameAssetRenderFitStatus status =
            WorkoutGameAssetRenderFitStatus::Unbound;
    QString assetId;
    QString variantKey;
    double obstacleAnchorMeters = 0.0;
    double obstacleStartDistanceMeters = 0.0;
    double obstacleEndDistanceMeters = 0.0;
    double assetStartDistanceMeters = 0.0;
    double forwardScale = 1.0;
    double upScale = 1.0;
    double forwardExtentMeters = 0.0;
    double upExtentMeters = 0.0;
    bool exactLegacy = false;
    double exactStartMeters = 0.0;
    double exactEndMeters = 0.0;
    double exactHeightMeters = 0.0;
};

struct WorkoutGameLegacyFt02Geometry
{
    WorkoutGameAssetRenderFitStatus status =
            WorkoutGameAssetRenderFitStatus::Unbound;
    bool enabled = false;
    double obstacleAnchorMeters = 0.0;
    double startMeters = 0.0;
    double endMeters = 0.0;
    double heightMeters = 0.0;
};

class WorkoutGameAssetPhysicsSampler
{
public:
    static WorkoutGameAssetPhysicsSample sample(
            const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
            std::size_t pieceIndex,
            double distanceMeters);

    static std::vector<double> breakpointsMeters(
            const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
            std::size_t pieceIndex);

    static std::vector<double> renderBreakpointsMeters(
            const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
            std::size_t pieceIndex);

    static WorkoutGameLegacyFt02Geometry legacyFt02Geometry(
            const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
            std::size_t pieceIndex);

    static WorkoutGameAssetRenderFit renderFit(
            const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
            std::size_t pieceIndex);

    static WorkoutGameAssetRenderTransform renderTransform(
            const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
            std::size_t pieceIndex);
};

#endif
