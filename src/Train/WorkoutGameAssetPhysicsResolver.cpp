/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameAssetPhysicsResolver.h"

#include "WorkoutGameAssetCatalog.h"
#include "WorkoutGameFt02PhysicsV1.h"
#include "WorkoutGameRoadCourse.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace {

constexpr double MaximumCourseDistanceMeters = 250000.0;

using Catalog = WorkoutGameAssetCatalog;
using Resolution = WorkoutGameAssetPhysicsResolution;
using Snapshot = WorkoutGameCourseAssetPhysicsSnapshot;
using Status = WorkoutGameAssetPhysicsResolveStatus;

Resolution failure(Status status)
{
    return {status, {}};
}

bool millimeters(double meters, std::int32_t &result)
{
    if (!std::isfinite(meters) || meters < 0.0
            || meters > MaximumCourseDistanceMeters) {
        return false;
    }
    const auto rounded = std::llround(meters * 1000.0);
    if (rounded < std::numeric_limits<std::int32_t>::min()
            || rounded > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    result = std::int32_t(rounded);
    return true;
}

std::int32_t scaledMillimeters(
        std::int32_t value,
        std::uint32_t resolvedExtent,
        std::int32_t nativeExtent)
{
    const std::int64_t numerator =
            std::int64_t(value) * std::int64_t(resolvedExtent);
    const std::int64_t denominator = nativeExtent;
    if (numerator >= 0) {
        return std::int32_t((numerator + denominator / 2) / denominator);
    }
    return std::int32_t(-((-numerator + denominator / 2) / denominator));
}

const Catalog::RouteProfileBinding *mainRoute(
        const Catalog::Asset &asset)
{
    for (const Catalog::RouteProfileBinding &binding :
            asset.physics.routeProfiles) {
        if (binding.routeKey == QStringLiteral("main")) return &binding;
    }
    return nullptr;
}

bool requiresFt02(const WorkoutGameRoadPiece &piece)
{
    return piece.terrain == WorkoutGameTerrainKind::LogOver
            && piece.challenge.enabled;
}

bool resolvedExtent(
        const Catalog::Profile &profile,
        double difficulty,
        std::uint32_t &extent)
{
    if (!std::isfinite(difficulty) || difficulty < 0.0 || difficulty > 1.0
            || !profile.difficultyScale.has_value()) {
        return false;
    }
    const Catalog::DifficultyScale &scale = *profile.difficultyScale;
    const std::int64_t result = std::int64_t(scale.baseExtentMm)
            + std::llround(double(scale.difficultyExtentMm) * difficulty);
    if (scale.nativeExtentMm <= 0 || result <= 0
            || result > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    extent = std::uint32_t(result);
    return true;
}

WorkoutGameAssetPhysicsDefinition definitionFor(
        const Catalog::Profile &profile,
        std::uint32_t extent)
{
    WorkoutGameAssetPhysicsDefinition definition =
            profile.profileId == QStringLiteral("FT-02-log-over-v1")
                && profile.profileVersion == 1
            ? WorkoutGameFt02PhysicsV1::definitionForExtent(extent)
            : WorkoutGameAssetPhysicsDefinition();
    definition.profileVersion = profile.profileVersion;
    definition.operation = profile.operation == Catalog::ProfileOperation::AddObstacle
            ? WorkoutGameAssetPhysicsOperation::AddObstacle
            : WorkoutGameAssetPhysicsOperation::ReplaceSurface;
    definition.coulombFrictionMilli = profile.surface.coulombFrictionMilli;
    definition.restitutionMilli = profile.surface.restitutionMilli;
    if (!definition.chains.empty()) return definition;
    definition.chains.reserve(std::size_t(profile.chains.size()));

    const std::int32_t nativeExtent =
            profile.difficultyScale->nativeExtentMm;
    for (const Catalog::Chain &sourceChain : profile.chains) {
        WorkoutGameAssetPhysicsChain chain;
        chain.points.reserve(std::size_t(sourceChain.points.size()));
        for (const Catalog::Point &sourcePoint : sourceChain.points) {
            chain.points.push_back({
                scaledMillimeters(sourcePoint.forwardMm, extent, nativeExtent),
                scaledMillimeters(sourcePoint.heightMm, extent, nativeExtent),
            });
        }
        definition.chains.push_back(std::move(chain));
    }
    return definition;
}

}

WorkoutGameAssetPhysicsResolution WorkoutGameAssetPhysicsResolver::resolve(
        const WorkoutGameAssetCatalog &catalog,
        const std::vector<WorkoutGameRoadPiece> &pieces)
{
    if (catalog.schemaVersion() != 1) {
        return failure(Status::UnsupportedCatalog);
    }

    WorkoutGameAssetPhysicsSnapshotBuilder builder(catalog.schemaVersion());
    const Catalog::Asset *ft02Asset = nullptr;
    const Catalog::RouteProfileBinding *ft02Route = nullptr;
    const Catalog::Profile *ft02Profile = nullptr;

    for (const WorkoutGameRoadPiece &piece : pieces) {
        WorkoutGameAssetPhysicsPieceBinding pieceBinding;
        std::int32_t geometryAnchorMm = 0;
        if (!millimeters(piece.geometryAnchorDistanceMeters,
                         geometryAnchorMm)) {
            return failure(Status::InvalidAnchor);
        }
        pieceBinding.obstacleAnchorMm = geometryAnchorMm;

        if (!requiresFt02(piece)) {
            if (!builder.appendPieceBinding(pieceBinding)) {
                return failure(Status::ResourceLimit);
            }
            continue;
        }

        std::int32_t challengeAnchorMm = 0;
        if (!millimeters(piece.challenge.obstacleDistanceMeters,
                         challengeAnchorMm)
                || challengeAnchorMm != geometryAnchorMm) {
            return failure(Status::InvalidAnchor);
        }
        pieceBinding.obstacleAnchorMm = challengeAnchorMm;

        if (!ft02Asset) {
            ft02Asset = catalog.findAsset(
                    QStringLiteral("FT-02-log-over-greybox"));
            if (!ft02Asset) return failure(Status::MissingAsset);
            ft02Route = mainRoute(*ft02Asset);
            if (!ft02Route) return failure(Status::MissingProfile);
            ft02Profile = catalog.findProfile(ft02Route->profileId);
            if (!ft02Profile || !ft02Profile->difficultyScale.has_value()) {
                return failure(Status::MissingProfile);
            }
        }

        std::uint32_t extent = 0;
        if (!resolvedExtent(*ft02Profile, piece.difficulty, extent)) {
            return failure(Status::InvalidDifficulty);
        }
        WorkoutGameAssetPhysicsDefinition definition =
                definitionFor(*ft02Profile, extent);
        std::uint32_t definitionIndex = Snapshot::NoIndex;
        if (!builder.internDefinition(definition, definitionIndex)) {
            return failure(Status::ResourceLimit);
        }

        WorkoutGameAssetPhysicsBinding assetBinding;
        assetBinding.assetId = ft02Asset->assetId;
        assetBinding.variantKey = ft02Route->variantKey;
        assetBinding.definitionIndex = definitionIndex;
        assetBinding.nativeForwardOriginMm =
                ft02Route->nativeForwardOriginMm;
        assetBinding.nativeForwardExtentMm =
                ft02Route->nativeForwardExtentMm;
        assetBinding.nativeUpExtentMm = ft02Route->nativeUpExtentMm;
        assetBinding.resolvedExtentMm = extent;
        std::uint32_t bindingIndex = Snapshot::NoIndex;
        if (!builder.internBinding(assetBinding, bindingIndex)) {
            return failure(Status::ResourceLimit);
        }

        pieceBinding.definitionIndex = definitionIndex;
        pieceBinding.bindingIndex = bindingIndex;
        if (!builder.appendPieceBinding(pieceBinding)) {
            return failure(Status::ResourceLimit);
        }
    }

    auto snapshot = builder.finish();
    if (!snapshot) return failure(Status::ResourceLimit);
    return {Status::Ready, std::move(snapshot)};
}
