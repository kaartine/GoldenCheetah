/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameAssetPhysicsSnapshot_h
#define _GC_WorkoutGameAssetPhysicsSnapshot_h

#include <QString>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

struct WorkoutGameRoadPlan;

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

struct WorkoutGameLegacyFt02Record
{
    static constexpr std::uint32_t CurrentVersion = 1;

    std::uint32_t recordVersion = CurrentVersion;
    bool enabled = false;
    double startMeters = 0.0;
    double endMeters = 0.0;
    double heightMeters = 0.0;
    double obstacleAnchorMeters = 0.0;
};

class WorkoutGameLegacyBinary64
{
public:
    static QString encode(double value);
    static bool decode(const QString &encoded, double &value);
};

struct WorkoutGameAssetPhysicsPieceBinding
{
    std::uint32_t definitionIndex = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t bindingIndex = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t legacyFt02RecordIndex =
            std::numeric_limits<std::uint32_t>::max();
    std::int32_t obstacleAnchorMm = 0;
    std::int16_t obstacleAnchorMicrometerRemainder = 0;
    std::uint32_t flags = 0;

    double obstacleAnchorMeters() const
    {
        return (double(obstacleAnchorMm) * 1000.0
                + obstacleAnchorMicrometerRemainder) / 1000000.0;
    }
};

struct WorkoutGameCourseAssetPhysicsSnapshot
{
    static constexpr std::uint32_t LegacyLayoutVersion = 1;
    static constexpr std::uint32_t CurrentVersion = 2;
    static constexpr std::uint32_t NoIndex =
            std::numeric_limits<std::uint32_t>::max();
    // Explicit adapter tag retained by schema-6 reads and schema-7 frozen
    // legacy records.
    static constexpr std::uint32_t LegacyProceduralV1 = 1U << 0;
    static constexpr std::uint32_t LegacyProcedural = LegacyProceduralV1;
    static constexpr std::size_t MaximumDefinitions = 64;
    static constexpr std::size_t MaximumBindings = 64;
    static constexpr std::size_t MaximumChainsPerDefinition = 8;
    static constexpr std::size_t MaximumPointsPerDefinition = 256;
    static constexpr std::size_t MaximumTotalPoints = 4096;
    static constexpr std::size_t MaximumPieceBindings = 4096;
    static constexpr std::size_t MaximumLegacyRecords = 4096;
    static constexpr std::size_t MaximumLegacyScalarFields = 32768;
    static constexpr qsizetype MaximumEncodedBytes = 256 * 1024;

    std::uint32_t snapshotVersion = CurrentVersion;
    std::uint32_t catalogSchemaVersion = 0; // 0 = no catalog/legacy, 1 = catalog v1.
    std::vector<WorkoutGameAssetPhysicsDefinition> physicsDefinitions;
    std::vector<WorkoutGameAssetPhysicsBinding> bindings;
    std::vector<WorkoutGameLegacyFt02Record> legacyFt02Records;
    std::vector<WorkoutGameAssetPhysicsPieceBinding> pieceBindings;
    // Read-only compatibility provenance. Writers still enforce the current
    // encoded-size limit after a v1 layout has expanded in memory.
    bool migratedFromLegacyLayout = false;
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
    explicit WorkoutGameAssetPhysicsSnapshotBuilder(
            std::uint32_t catalogSchemaVersion = 0)
    {
        snapshot_.catalogSchemaVersion = catalogSchemaVersion;
    }

    bool internDefinition(
            const WorkoutGameAssetPhysicsDefinition &definition,
            std::uint32_t &index);
    bool internBinding(
            const WorkoutGameAssetPhysicsBinding &binding,
            std::uint32_t &index);
    bool internLegacyFt02Record(
            const WorkoutGameLegacyFt02Record &record,
            std::uint32_t &index);
    bool appendPieceBinding(
            const WorkoutGameAssetPhysicsPieceBinding &binding);
    std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot> finish() const;

    static std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot>
            legacyFor(const WorkoutGameRoadPlan &plan);
    static std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot>
            frozenLegacyFt02For(const WorkoutGameRoadPlan &plan);

private:
    WorkoutGameCourseAssetPhysicsSnapshot snapshot_;
};

#endif
