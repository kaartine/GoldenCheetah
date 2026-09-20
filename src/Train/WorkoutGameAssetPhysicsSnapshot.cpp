/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameAssetPhysicsSnapshot.h"
#include "WorkoutGameRoadPlan.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double MaximumCourseDistanceMeters = 250000.0;
constexpr std::int32_t MaximumForwardCoordinateMm = 64000;
constexpr std::int32_t MaximumHeightCoordinateMm = 16000;
constexpr std::int64_t MinimumBox2DSegmentLengthSquaredMm = 25;

bool samePoint(
        const WorkoutGameAssetPhysicsPoint &left,
        const WorkoutGameAssetPhysicsPoint &right)
{
    return left.forwardMm == right.forwardMm
            && left.heightMm == right.heightMm;
}

bool sameDefinition(
        const WorkoutGameAssetPhysicsDefinition &left,
        const WorkoutGameAssetPhysicsDefinition &right)
{
    if (left.profileVersion != right.profileVersion
            || left.operation != right.operation
            || left.coulombFrictionMilli != right.coulombFrictionMilli
            || left.restitutionMilli != right.restitutionMilli
            || left.chains.size() != right.chains.size()) {
        return false;
    }
    for (std::size_t chainIndex = 0;
            chainIndex < left.chains.size(); ++chainIndex) {
        const auto &leftPoints = left.chains[chainIndex].points;
        const auto &rightPoints = right.chains[chainIndex].points;
        if (leftPoints.size() != rightPoints.size()
                || !std::equal(leftPoints.begin(), leftPoints.end(),
                    rightPoints.begin(), samePoint)) {
            return false;
        }
    }
    return true;
}

bool sameBinding(
        const WorkoutGameAssetPhysicsBinding &left,
        const WorkoutGameAssetPhysicsBinding &right)
{
    return left.assetId == right.assetId
            && left.variantKey == right.variantKey
            && left.definitionIndex == right.definitionIndex
            && left.nativeForwardOriginMm == right.nativeForwardOriginMm
            && left.nativeForwardExtentMm == right.nativeForwardExtentMm
            && left.nativeUpExtentMm == right.nativeUpExtentMm
            && left.resolvedExtentMm == right.resolvedExtentMm;
}

bool portableKey(const QString &value)
{
    if (value.isEmpty() || value.size() > 128) return false;
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (code < 0x21 || code > 0x7e) return false;
    }
    return true;
}

bool validBinding(
        const WorkoutGameAssetPhysicsBinding &binding,
        std::size_t definitionCount)
{
    using Snapshot = WorkoutGameCourseAssetPhysicsSnapshot;
    return portableKey(binding.assetId)
            && (binding.variantKey.isEmpty() || portableKey(binding.variantKey))
            && (binding.definitionIndex == Snapshot::NoIndex
                || binding.definitionIndex < definitionCount)
            && std::abs(std::int64_t(binding.nativeForwardOriginMm))
                <= MaximumForwardCoordinateMm
            && binding.nativeForwardExtentMm > 0
            && binding.nativeForwardExtentMm
                <= std::uint32_t(MaximumForwardCoordinateMm * 2)
            && binding.nativeUpExtentMm > 0
            && binding.nativeUpExtentMm
                <= std::uint32_t(MaximumHeightCoordinateMm)
            && binding.resolvedExtentMm > 0
            && binding.resolvedExtentMm
                <= std::uint32_t(MaximumForwardCoordinateMm * 2);
}

bool validPieceBinding(
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
        const WorkoutGameAssetPhysicsPieceBinding &piece)
{
    using Snapshot = WorkoutGameCourseAssetPhysicsSnapshot;
    if ((piece.flags & ~Snapshot::LegacyProceduralV1) != 0
            || piece.obstacleAnchorMm < 0
            || piece.obstacleAnchorMm > 250000000
            || piece.obstacleAnchorMicrometerRemainder < -500
            || piece.obstacleAnchorMicrometerRemainder > 500) {
        return false;
    }
    const bool hasPhysics = piece.definitionIndex != Snapshot::NoIndex;
    const bool hasAsset = piece.bindingIndex != Snapshot::NoIndex;
    if ((piece.flags & Snapshot::LegacyProceduralV1) != 0
            && (hasPhysics || hasAsset)) {
        return false;
    }
    if (hasPhysics
            && piece.definitionIndex >= snapshot.physicsDefinitions.size()) {
        return false;
    }
    if (hasAsset) {
        if (piece.bindingIndex >= snapshot.bindings.size()) return false;
        const auto definition =
                snapshot.bindings[piece.bindingIndex].definitionIndex;
        if (definition != Snapshot::NoIndex
                && definition != piece.definitionIndex) {
            return false;
        }
    }
    return true;
}

WorkoutGameAssetPhysicsSnapshotValidationStatus validateDefinition(
        const WorkoutGameAssetPhysicsDefinition &definition,
        std::size_t &totalPoints)
{
    using Snapshot = WorkoutGameCourseAssetPhysicsSnapshot;
    using Status = WorkoutGameAssetPhysicsSnapshotValidationStatus;
    if (definition.chains.size() > Snapshot::MaximumChainsPerDefinition) {
        return Status::ResourceLimit;
    }
    if (definition.profileVersion
            != WorkoutGameAssetPhysicsDefinition::CurrentProfileVersion) {
        return Status::UnsupportedVersion;
    }
    if (definition.operation
                    != WorkoutGameAssetPhysicsOperation::AddObstacle
            || definition.coulombFrictionMilli > 2000
            || definition.restitutionMilli > 250
            || definition.chains.size() != 1) {
        return Status::InvalidSnapshot;
    }

    std::size_t definitionPoints = 0;
    std::int32_t previousChainEnd = 0;
    bool havePreviousChain = false;
    for (const WorkoutGameAssetPhysicsChain &chain : definition.chains) {
        if (chain.points.size() < 2) return Status::InvalidSnapshot;
        definitionPoints += chain.points.size();
        totalPoints += chain.points.size();
        if (definitionPoints > Snapshot::MaximumPointsPerDefinition
                || totalPoints > Snapshot::MaximumTotalPoints) {
            return Status::ResourceLimit;
        }
        if (havePreviousChain
                && chain.points.front().forwardMm <= previousChainEnd) {
            return Status::InvalidSnapshot;
        }
        for (std::size_t pointIndex = 0;
                pointIndex < chain.points.size(); ++pointIndex) {
            const WorkoutGameAssetPhysicsPoint &point =
                    chain.points[pointIndex];
            if (std::abs(std::int64_t(point.forwardMm))
                        > MaximumForwardCoordinateMm
                    || std::abs(std::int64_t(point.heightMm))
                        > MaximumHeightCoordinateMm
                    || (pointIndex > 0
                        && point.forwardMm
                            <= chain.points[pointIndex - 1].forwardMm)) {
                return Status::InvalidSnapshot;
            }
            if (pointIndex > 0) {
                const WorkoutGameAssetPhysicsPoint &previous =
                        chain.points[pointIndex - 1];
                const std::int64_t forward =
                        std::int64_t(point.forwardMm) - previous.forwardMm;
                const std::int64_t height =
                        std::int64_t(point.heightMm) - previous.heightMm;
                if (forward * forward + height * height
                        <= MinimumBox2DSegmentLengthSquaredMm) {
                    return Status::InvalidSnapshot;
                }
            }
        }
        if (definition.operation
                    == WorkoutGameAssetPhysicsOperation::AddObstacle
                && (chain.points.front().heightMm != 0
                    || chain.points.back().heightMm != 0)) {
            return Status::InvalidSnapshot;
        }
        previousChainEnd = chain.points.back().forwardMm;
        havePreviousChain = true;
    }
    return Status::Ready;
}

}

WorkoutGameAssetPhysicsSnapshotValidationStatus
WorkoutGameAssetPhysicsSnapshotValidator::validate(
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
        std::size_t roadPieceCount)
{
    using Snapshot = WorkoutGameCourseAssetPhysicsSnapshot;
    using Status = WorkoutGameAssetPhysicsSnapshotValidationStatus;
    if (snapshot.snapshotVersion != Snapshot::CurrentVersion
            || snapshot.catalogSchemaVersion > 1) {
        return Status::UnsupportedVersion;
    }
    if (snapshot.physicsDefinitions.size() > Snapshot::MaximumDefinitions
            || snapshot.bindings.size() > Snapshot::MaximumBindings
            || snapshot.pieceBindings.size() > Snapshot::MaximumPieceBindings) {
        return Status::ResourceLimit;
    }
    if (snapshot.pieceBindings.size() != roadPieceCount) {
        return Status::InvalidSnapshot;
    }

    std::size_t totalPoints = 0;
    for (const WorkoutGameAssetPhysicsDefinition &definition :
            snapshot.physicsDefinitions) {
        const Status status = validateDefinition(definition, totalPoints);
        if (status != Status::Ready) return status;
    }
    for (const WorkoutGameAssetPhysicsBinding &binding : snapshot.bindings) {
        if (!validBinding(binding, snapshot.physicsDefinitions.size())) {
            return Status::InvalidSnapshot;
        }
    }
    for (const WorkoutGameAssetPhysicsPieceBinding &binding :
            snapshot.pieceBindings) {
        if (!validPieceBinding(snapshot, binding)) {
            return Status::InvalidSnapshot;
        }
    }

    // Canonical arrays are unique and ordered by their first use by a piece.
    for (std::size_t i = 0; i < snapshot.physicsDefinitions.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (sameDefinition(snapshot.physicsDefinitions[i],
                               snapshot.physicsDefinitions[j])) {
                return Status::InvalidSnapshot;
            }
        }
    }
    for (std::size_t i = 0; i < snapshot.bindings.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (sameBinding(snapshot.bindings[i], snapshot.bindings[j])) {
                return Status::InvalidSnapshot;
            }
        }
    }

    std::size_t nextBinding = 0;
    std::size_t nextDefinition = 0;
    for (const WorkoutGameAssetPhysicsPieceBinding &piece :
            snapshot.pieceBindings) {
        if (piece.bindingIndex != Snapshot::NoIndex) {
            if (piece.bindingIndex > nextBinding) return Status::InvalidSnapshot;
            if (piece.bindingIndex == nextBinding) ++nextBinding;
        }
        if (piece.definitionIndex == Snapshot::NoIndex) continue;
        if (piece.definitionIndex > nextDefinition) {
            return Status::InvalidSnapshot;
        }
        if (piece.definitionIndex == nextDefinition) ++nextDefinition;
    }
    if (nextBinding != snapshot.bindings.size()
            || nextDefinition != snapshot.physicsDefinitions.size()) {
        return Status::InvalidSnapshot;
    }
    return Status::Ready;
}

bool WorkoutGameAssetPhysicsSnapshotBuilder::internDefinition(
        const WorkoutGameAssetPhysicsDefinition &definition,
        std::uint32_t &index)
{
    std::size_t totalPoints = 0;
    if (validateDefinition(definition, totalPoints)
            != WorkoutGameAssetPhysicsSnapshotValidationStatus::Ready) {
        return false;
    }
    const auto existing = std::find_if(
            snapshot_.physicsDefinitions.begin(),
            snapshot_.physicsDefinitions.end(),
            [&definition](const auto &candidate) {
                return sameDefinition(candidate, definition);
            });
    if (existing != snapshot_.physicsDefinitions.end()) {
        index = std::uint32_t(std::distance(
                snapshot_.physicsDefinitions.begin(), existing));
        return true;
    }
    if (snapshot_.physicsDefinitions.size()
            >= WorkoutGameCourseAssetPhysicsSnapshot::MaximumDefinitions) {
        return false;
    }
    std::size_t existingPoints = 0;
    for (const auto &candidate : snapshot_.physicsDefinitions) {
        for (const auto &chain : candidate.chains) {
            existingPoints += chain.points.size();
        }
    }
    if (existingPoints + totalPoints
            > WorkoutGameCourseAssetPhysicsSnapshot::MaximumTotalPoints) {
        return false;
    }
    index = std::uint32_t(snapshot_.physicsDefinitions.size());
    snapshot_.physicsDefinitions.push_back(definition);
    return true;
}

bool WorkoutGameAssetPhysicsSnapshotBuilder::internBinding(
        const WorkoutGameAssetPhysicsBinding &binding,
        std::uint32_t &index)
{
    const auto existing = std::find_if(
            snapshot_.bindings.begin(), snapshot_.bindings.end(),
            [&binding](const auto &candidate) {
                return sameBinding(candidate, binding);
            });
    if (existing != snapshot_.bindings.end()) {
        index = std::uint32_t(std::distance(
                snapshot_.bindings.begin(), existing));
        return true;
    }
    if (snapshot_.bindings.size()
            >= WorkoutGameCourseAssetPhysicsSnapshot::MaximumBindings
            || !validBinding(binding, snapshot_.physicsDefinitions.size())) {
        return false;
    }
    index = std::uint32_t(snapshot_.bindings.size());
    snapshot_.bindings.push_back(binding);
    return true;
}

bool WorkoutGameAssetPhysicsSnapshotBuilder::appendPieceBinding(
        const WorkoutGameAssetPhysicsPieceBinding &binding)
{
    if (snapshot_.pieceBindings.size()
                >= WorkoutGameCourseAssetPhysicsSnapshot::MaximumPieceBindings
            || !validPieceBinding(snapshot_, binding)) {
        return false;
    }
    snapshot_.pieceBindings.push_back(binding);
    return true;
}

std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot>
WorkoutGameAssetPhysicsSnapshotBuilder::finish() const
{
    if (WorkoutGameAssetPhysicsSnapshotValidator::validate(
                snapshot_, snapshot_.pieceBindings.size())
            != WorkoutGameAssetPhysicsSnapshotValidationStatus::Ready) {
        return {};
    }
    return std::make_shared<const WorkoutGameCourseAssetPhysicsSnapshot>(
            snapshot_);
}

std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot>
WorkoutGameAssetPhysicsSnapshotBuilder::legacyFor(
        const WorkoutGameRoadPlan &plan)
{
    WorkoutGameAssetPhysicsSnapshotBuilder builder;
    for (const WorkoutGameRoadPiece &piece : plan.pieces) {
        WorkoutGameAssetPhysicsPieceBinding binding;
        binding.flags = WorkoutGameCourseAssetPhysicsSnapshot::LegacyProcedural;
        if (!std::isfinite(piece.geometryAnchorDistanceMeters)
                || piece.geometryAnchorDistanceMeters < 0.0
                || piece.geometryAnchorDistanceMeters
                    > MaximumCourseDistanceMeters) {
            return {};
        }
        binding.obstacleAnchorMm = std::int32_t(std::llround(
                piece.geometryAnchorDistanceMeters * 1000.0));
        binding.obstacleAnchorMicrometerRemainder = std::int16_t(std::llround(
                piece.geometryAnchorDistanceMeters * 1000000.0)
                - std::int64_t(binding.obstacleAnchorMm) * 1000);
        if (!builder.appendPieceBinding(binding)) return {};
    }
    return builder.finish();
}
