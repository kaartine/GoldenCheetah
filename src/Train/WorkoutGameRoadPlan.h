/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRoadPlan_h
#define _GC_WorkoutGameRoadPlan_h

#include "WorkoutGameRoadCourse.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

enum class WorkoutGameAssetPhysicsOperation : std::uint8_t
{
    AddObstacle = 1,
    ReplaceSurface = 2
};

struct WorkoutGameAssetPhysicsPoint
{
    std::int32_t forwardMm = 0;
    std::int32_t heightMm = 0;
};

struct WorkoutGameAssetPhysicsChain
{
    std::vector<WorkoutGameAssetPhysicsPoint> points;
};

struct WorkoutGameAssetPhysicsDefinition
{
    static constexpr std::uint32_t CurrentProfileVersion = 1;

    std::uint32_t profileVersion = CurrentProfileVersion;
    WorkoutGameAssetPhysicsOperation operation =
            WorkoutGameAssetPhysicsOperation::AddObstacle;
    std::uint16_t coulombFrictionMilli = 0;
    std::uint16_t restitutionMilli = 0;
    std::vector<WorkoutGameAssetPhysicsChain> chains;
};

struct WorkoutGameAssetPhysicsBinding
{
    QString assetId;
    QString variantKey;
    std::uint32_t definitionIndex = std::numeric_limits<std::uint32_t>::max();
    std::int32_t nativeForwardOriginMm = 0;
    std::uint32_t nativeForwardExtentMm = 0;
    std::uint32_t nativeUpExtentMm = 0;
    std::uint32_t resolvedExtentMm = 0;
};

struct WorkoutGameAssetPhysicsPieceBinding
{
    std::uint32_t definitionIndex = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t bindingIndex = std::numeric_limits<std::uint32_t>::max();
    std::int32_t obstacleAnchorMm = 0;
    std::uint32_t flags = 0;
};

struct WorkoutGameCourseAssetPhysicsSnapshot
{
    static constexpr std::uint32_t CurrentVersion = 1;
    static constexpr std::uint32_t NoIndex =
            std::numeric_limits<std::uint32_t>::max();
    // Explicit legacy adapter tag; activation of frozen legacy evaluation is
    // separate from this preparatory persistence model.
    static constexpr std::uint32_t LegacyProceduralV1 = 1U << 0;
    static constexpr std::uint32_t LegacyProcedural = LegacyProceduralV1;
    static constexpr std::size_t MaximumDefinitions = 64;
    static constexpr std::size_t MaximumBindings = 64;
    static constexpr std::size_t MaximumChainsPerDefinition = 8;
    static constexpr std::size_t MaximumPointsPerDefinition = 256;
    static constexpr std::size_t MaximumTotalPoints = 4096;
    static constexpr std::size_t MaximumPieceBindings = 4096;
    static constexpr qsizetype MaximumEncodedBytes = 256 * 1024;

    std::uint32_t snapshotVersion = CurrentVersion;
    std::uint32_t catalogSchemaVersion = 0; // 0 = no catalog/legacy, 1 = catalog v1.
    std::vector<WorkoutGameAssetPhysicsDefinition> physicsDefinitions;
    std::vector<WorkoutGameAssetPhysicsBinding> bindings;
    std::vector<WorkoutGameAssetPhysicsPieceBinding> pieceBindings;
};

enum class WorkoutGameAssetPhysicsSnapshotValidationStatus
{
    Ready,
    UnsupportedVersion,
    ResourceLimit,
    InvalidSnapshot
};

class WorkoutGameAssetPhysicsSnapshotValidator
{
public:
    static WorkoutGameAssetPhysicsSnapshotValidationStatus validate(
            const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
            std::size_t roadPieceCount);
};

class WorkoutGameAssetPhysicsSnapshotBuilder
{
public:
    explicit WorkoutGameAssetPhysicsSnapshotBuilder(std::uint32_t catalogSchemaVersion = 0)
    {
        snapshot_.catalogSchemaVersion = catalogSchemaVersion;
    }
    bool internDefinition(
            const WorkoutGameAssetPhysicsDefinition &definition,
            std::uint32_t &index);
    bool internBinding(
            const WorkoutGameAssetPhysicsBinding &binding,
            std::uint32_t &index);
    bool appendPieceBinding(
            const WorkoutGameAssetPhysicsPieceBinding &binding);
    std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot> finish() const;

    static std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot>
            legacyFor(const struct WorkoutGameRoadPlan &plan);

private:
    WorkoutGameCourseAssetPhysicsSnapshot snapshot_;
};

struct WorkoutGameRoadPlan
{
    static constexpr std::uint32_t LegacyGenerationVersion = 1;
    static constexpr std::uint32_t BankAndReliefGenerationVersion = 2;
    static constexpr std::uint32_t CurrentGenerationVersion = 3;
    static constexpr std::size_t MaximumPieces = 4096;

    std::uint32_t generationVersion = CurrentGenerationVersion;
    std::vector<WorkoutGameRoadPiece> pieces;
    std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot>
            assetPhysicsSnapshot;
};

enum class WorkoutGameRoadPlanValidationStatus
{
    Ready,
    UnsupportedVersion,
    ResourceLimit,
    InvalidPlan
};

class WorkoutGameRoadPlanValidator
{
public:
    static WorkoutGameRoadPlanValidationStatus validate(
            const WorkoutGameRoadPlan &plan,
            std::size_t sourceSectionCount);
};

#endif
