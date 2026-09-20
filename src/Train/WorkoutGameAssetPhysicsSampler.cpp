/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameAssetPhysicsSampler.h"

#include <algorithm>
#include <cmath>

namespace {

const WorkoutGameAssetPhysicsDefinition *definitionAt(
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
        std::size_t pieceIndex,
        const WorkoutGameAssetPhysicsPieceBinding *&pieceBinding)
{
    using Snapshot = WorkoutGameCourseAssetPhysicsSnapshot;
    pieceBinding = nullptr;
    if (pieceIndex >= snapshot.pieceBindings.size()) return nullptr;
    const auto &piece = snapshot.pieceBindings[pieceIndex];
    if ((piece.flags & Snapshot::LegacyProceduralV1) != 0
            || piece.definitionIndex == Snapshot::NoIndex
            || piece.definitionIndex >= snapshot.physicsDefinitions.size()) {
        return nullptr;
    }
    pieceBinding = &piece;
    return &snapshot.physicsDefinitions[piece.definitionIndex];
}

}

WorkoutGameAssetPhysicsSample WorkoutGameAssetPhysicsSampler::sample(
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
        std::size_t pieceIndex,
        double distanceMeters)
{
    WorkoutGameAssetPhysicsSample result;
    const WorkoutGameAssetPhysicsPieceBinding *piece = nullptr;
    const WorkoutGameAssetPhysicsDefinition *definition =
            definitionAt(snapshot, pieceIndex, piece);
    if (!definition || !std::isfinite(distanceMeters)) return result;

    result.bound = true;
    result.operation = definition->operation;
    result.coulombFriction =
            double(definition->coulombFrictionMilli) / 1000.0;
    result.restitution = double(definition->restitutionMilli) / 1000.0;
    double localMm = distanceMeters * 1000.0
            - double(piece->obstacleAnchorMm);
    const double nearestMillimeter = std::round(localMm);
    if (std::abs(localMm - nearestMillimeter) < 1.0e-7) {
        localMm = nearestMillimeter;
    }

    for (const WorkoutGameAssetPhysicsChain &chain : definition->chains) {
        if (chain.points.size() < 2
                || localMm < chain.points.front().forwardMm
                || localMm > chain.points.back().forwardMm) {
            continue;
        }
        const auto right = std::lower_bound(
                chain.points.begin(), chain.points.end(), localMm,
                [](const WorkoutGameAssetPhysicsPoint &point, double value) {
                    return point.forwardMm < value;
                });
        double heightMm = 0.0;
        if (right == chain.points.begin()) {
            heightMm = right->heightMm;
        } else if (right == chain.points.end()) {
            heightMm = chain.points.back().heightMm;
        } else {
            const auto &left = *(right - 1);
            const double span = double(right->forwardMm - left.forwardMm);
            const double progress = (localMm - left.forwardMm) / span;
            heightMm = left.heightMm
                    + progress * double(right->heightMm - left.heightMm);
        }
        result.surfacePresent = true;
        result.offsetMeters = heightMm / 1000.0;
        return result;
    }
    return result;
}

std::vector<double> WorkoutGameAssetPhysicsSampler::breakpointsMeters(
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
        std::size_t pieceIndex)
{
    std::vector<double> result;
    const WorkoutGameAssetPhysicsPieceBinding *piece = nullptr;
    const WorkoutGameAssetPhysicsDefinition *definition =
            definitionAt(snapshot, pieceIndex, piece);
    if (!definition) return result;

    for (const WorkoutGameAssetPhysicsChain &chain : definition->chains) {
        for (const WorkoutGameAssetPhysicsPoint &point : chain.points) {
            result.push_back((double(piece->obstacleAnchorMm)
                    + point.forwardMm) / 1000.0);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

WorkoutGameAssetRenderFit WorkoutGameAssetPhysicsSampler::renderFit(
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
        std::size_t pieceIndex)
{
    using Snapshot = WorkoutGameCourseAssetPhysicsSnapshot;
    WorkoutGameAssetRenderFit result;
    if (pieceIndex >= snapshot.pieceBindings.size()) {
        result.status = WorkoutGameAssetRenderFitStatus::Invalid;
        return result;
    }
    const WorkoutGameAssetPhysicsPieceBinding &piece =
            snapshot.pieceBindings[pieceIndex];
    if ((piece.flags & Snapshot::LegacyProceduralV1) != 0
            || (piece.definitionIndex == Snapshot::NoIndex
                && piece.bindingIndex == Snapshot::NoIndex)) {
        return result;
    }
    if (piece.definitionIndex == Snapshot::NoIndex
            || piece.bindingIndex == Snapshot::NoIndex
            || piece.bindingIndex >= snapshot.bindings.size()) {
        result.status = WorkoutGameAssetRenderFitStatus::Invalid;
        return result;
    }
    const WorkoutGameAssetPhysicsBinding &binding =
            snapshot.bindings[piece.bindingIndex];
    if (binding.definitionIndex != piece.definitionIndex
            || binding.nativeForwardExtentMm == 0
            || binding.nativeUpExtentMm == 0
            || binding.resolvedExtentMm == 0) {
        result.status = WorkoutGameAssetRenderFitStatus::Invalid;
        return result;
    }
    result.status = WorkoutGameAssetRenderFitStatus::Ready;
    result.assetId = binding.assetId;
    result.variantKey = binding.variantKey;
    result.obstacleAnchorMm = piece.obstacleAnchorMm;
    result.nativeForwardOriginMm = binding.nativeForwardOriginMm;
    result.nativeForwardExtentMm = binding.nativeForwardExtentMm;
    result.nativeUpExtentMm = binding.nativeUpExtentMm;
    result.resolvedExtentMm = binding.resolvedExtentMm;
    return result;
}
