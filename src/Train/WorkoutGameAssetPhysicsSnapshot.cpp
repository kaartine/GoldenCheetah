/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameAssetPhysicsSnapshot.h"
#include "WorkoutGameLegacyFt02V1.h"
#include "WorkoutGameRoadPlan.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr double MaximumCourseDistanceMeters = 250000.0;
constexpr std::int32_t MaximumForwardCoordinateMm = 64000;
constexpr std::int32_t MaximumHeightCoordinateMm = 16000;
constexpr std::int64_t MinimumBox2DSegmentLengthSquaredMm = 25;
constexpr std::size_t LegacyFt02ScalarFieldsPerRecord = 4;

static_assert(sizeof(double) == sizeof(std::uint64_t),
              "legacy FT02 persistence requires binary64 doubles");
static_assert(std::numeric_limits<double>::is_iec559,
              "legacy FT02 persistence requires IEC 559 doubles");

std::uint64_t binary64Bits(double value)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool sameLegacyFt02Record(
        const WorkoutGameLegacyFt02Record &left,
        const WorkoutGameLegacyFt02Record &right)
{
    return left.recordVersion == right.recordVersion
            && left.enabled == right.enabled
            && binary64Bits(left.startMeters) == binary64Bits(right.startMeters)
            && binary64Bits(left.endMeters) == binary64Bits(right.endMeters)
            && binary64Bits(left.heightMeters) == binary64Bits(right.heightMeters)
            && binary64Bits(left.obstacleAnchorMeters)
                == binary64Bits(right.obstacleAnchorMeters);
}

WorkoutGameAssetPhysicsSnapshotValidationStatus validateLegacyFt02Record(
        const WorkoutGameLegacyFt02Record &record)
{
    using Status = WorkoutGameAssetPhysicsSnapshotValidationStatus;
    if (record.recordVersion != WorkoutGameLegacyFt02Record::CurrentVersion) {
        return Status::UnsupportedVersion;
    }
    if (!std::isfinite(record.startMeters)
            || !std::isfinite(record.endMeters)
            || !std::isfinite(record.heightMeters)
            || !std::isfinite(record.obstacleAnchorMeters)
            || record.startMeters >= record.endMeters
            || std::abs(record.startMeters) > 64.0
            || std::abs(record.endMeters) > 64.0
            || record.heightMeters <= 0.0
            || record.heightMeters > 16.0
            || record.obstacleAnchorMeters < 0.0
            || record.obstacleAnchorMeters > MaximumCourseDistanceMeters) {
        return Status::InvalidSnapshot;
    }
    return Status::Ready;
}

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
    const bool hasLegacy = piece.legacyFt02RecordIndex != Snapshot::NoIndex;
    if ((piece.flags & Snapshot::LegacyProceduralV1) != 0
            && (hasPhysics || hasAsset)) {
        return false;
    }
    if (hasLegacy
            && ((piece.flags & Snapshot::LegacyProceduralV1) == 0
                || piece.legacyFt02RecordIndex
                    >= snapshot.legacyFt02Records.size())) {
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

QString WorkoutGameLegacyBinary64::encode(double value)
{
    if (!std::isfinite(value)) return {};
    constexpr char Digits[] = "0123456789abcdef";
    const std::uint64_t bits = binary64Bits(value);
    QString result(16, QLatin1Char('0'));
    for (int index = 0; index < result.size(); ++index) {
        const int shift = (15 - index) * 4;
        result[index] = QLatin1Char(Digits[(bits >> shift) & 0x0f]);
    }
    return result;
}

bool WorkoutGameLegacyBinary64::decode(
        const QString &encoded,
        double &value)
{
    if (encoded.size() != 16) return false;
    std::uint64_t bits = 0;
    for (const QChar character : encoded) {
        const ushort code = character.unicode();
        std::uint64_t nibble = 0;
        if (code >= '0' && code <= '9') {
            nibble = code - '0';
        } else if (code >= 'a' && code <= 'f') {
            nibble = code - 'a' + 10;
        } else {
            return false;
        }
        bits = (bits << 4) | nibble;
    }
    double decoded = 0.0;
    std::memcpy(&decoded, &bits, sizeof(decoded));
    if (!std::isfinite(decoded)) return false;
    value = decoded;
    return true;
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
            || snapshot.legacyFt02Records.size()
                > Snapshot::MaximumLegacyRecords
            || snapshot.pieceBindings.size() > Snapshot::MaximumPieceBindings) {
        return Status::ResourceLimit;
    }
    if (snapshot.legacyFt02Records.size()
            > Snapshot::MaximumLegacyScalarFields
                / LegacyFt02ScalarFieldsPerRecord) {
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
    for (const WorkoutGameLegacyFt02Record &record :
            snapshot.legacyFt02Records) {
        const Status status = validateLegacyFt02Record(record);
        if (status != Status::Ready) return status;
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
    for (std::size_t i = 0; i < snapshot.legacyFt02Records.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (sameLegacyFt02Record(snapshot.legacyFt02Records[i],
                                     snapshot.legacyFt02Records[j])) {
                return Status::InvalidSnapshot;
            }
        }
    }

    std::size_t nextBinding = 0;
    std::size_t nextDefinition = 0;
    std::size_t nextLegacyRecord = 0;
    for (const WorkoutGameAssetPhysicsPieceBinding &piece :
            snapshot.pieceBindings) {
        if (piece.bindingIndex != Snapshot::NoIndex) {
            if (piece.bindingIndex > nextBinding) return Status::InvalidSnapshot;
            if (piece.bindingIndex == nextBinding) ++nextBinding;
        }
        if (piece.definitionIndex != Snapshot::NoIndex) {
            if (piece.definitionIndex > nextDefinition) {
                return Status::InvalidSnapshot;
            }
            if (piece.definitionIndex == nextDefinition) ++nextDefinition;
        }
        if (piece.legacyFt02RecordIndex != Snapshot::NoIndex) {
            if (piece.legacyFt02RecordIndex > nextLegacyRecord) {
                return Status::InvalidSnapshot;
            }
            if (piece.legacyFt02RecordIndex == nextLegacyRecord) {
                ++nextLegacyRecord;
            }
        }
    }
    if (nextBinding != snapshot.bindings.size()
            || nextDefinition != snapshot.physicsDefinitions.size()
            || nextLegacyRecord != snapshot.legacyFt02Records.size()) {
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

bool WorkoutGameAssetPhysicsSnapshotBuilder::internLegacyFt02Record(
        const WorkoutGameLegacyFt02Record &record,
        std::uint32_t &index)
{
    if (validateLegacyFt02Record(record)
            != WorkoutGameAssetPhysicsSnapshotValidationStatus::Ready) {
        return false;
    }
    const auto existing = std::find_if(
            snapshot_.legacyFt02Records.begin(),
            snapshot_.legacyFt02Records.end(),
            [&record](const auto &candidate) {
                return sameLegacyFt02Record(candidate, record);
            });
    if (existing != snapshot_.legacyFt02Records.end()) {
        index = std::uint32_t(std::distance(
                snapshot_.legacyFt02Records.begin(), existing));
        return true;
    }
    if (snapshot_.legacyFt02Records.size()
            >= WorkoutGameCourseAssetPhysicsSnapshot::MaximumLegacyRecords) {
        return false;
    }
    index = std::uint32_t(snapshot_.legacyFt02Records.size());
    snapshot_.legacyFt02Records.push_back(record);
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

std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot>
WorkoutGameAssetPhysicsSnapshotBuilder::frozenLegacyFt02For(
        const WorkoutGameRoadPlan &plan)
{
    WorkoutGameAssetPhysicsSnapshotBuilder builder;
    for (const WorkoutGameRoadPiece &piece : plan.pieces) {
        WorkoutGameAssetPhysicsPieceBinding binding;
        binding.flags = WorkoutGameCourseAssetPhysicsSnapshot::LegacyProceduralV1;
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

        if (piece.terrain == WorkoutGameTerrainKind::LogOver) {
            const double radius =
                    WorkoutGameLegacyFt02V1::radiusMeters(piece.difficulty);
            WorkoutGameLegacyFt02Record record;
            record.enabled = piece.challenge.enabled;
            record.startMeters = -radius;
            record.endMeters = radius;
            record.heightMeters = 2.0 * radius;
            record.obstacleAnchorMeters =
                    piece.challenge.obstacleDistanceMeters;
            if (!builder.internLegacyFt02Record(
                        record, binding.legacyFt02RecordIndex)) {
                return {};
            }
        }
        if (!builder.appendPieceBinding(binding)) return {};
    }
    return builder.finish();
}
