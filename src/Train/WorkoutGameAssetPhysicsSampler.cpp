/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameAssetPhysicsSampler.h"
#include "WorkoutGameLegacyFt02V1.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double Ft02NativeExtentMeters = 0.54;
constexpr double Ft02NativeDeadZoneMeters = 0.75;

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

const WorkoutGameLegacyFt02Record *legacyFt02RecordAt(
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
        std::size_t pieceIndex)
{
    using Snapshot = WorkoutGameCourseAssetPhysicsSnapshot;
    if (pieceIndex >= snapshot.pieceBindings.size()) return nullptr;
    const auto &piece = snapshot.pieceBindings[pieceIndex];
    if ((piece.flags & Snapshot::LegacyProceduralV1) == 0
            || piece.legacyFt02RecordIndex == Snapshot::NoIndex
            || piece.legacyFt02RecordIndex >= snapshot.legacyFt02Records.size()) {
        return nullptr;
    }
    const auto &record = snapshot.legacyFt02Records[
            piece.legacyFt02RecordIndex];
    if (record.recordVersion != WorkoutGameLegacyFt02Record::CurrentVersion
            || !std::isfinite(record.startMeters)
            || !std::isfinite(record.endMeters)
            || !std::isfinite(record.heightMeters)
            || !std::isfinite(record.obstacleAnchorMeters)
            || record.startMeters >= record.endMeters
            || std::abs(record.startMeters) > 64.0
            || std::abs(record.endMeters) > 64.0
            || record.heightMeters <= 0.0
            || record.heightMeters > 16.0
            || (record.enabled
                && (record.obstacleAnchorMeters < 0.0
                    || record.obstacleAnchorMeters > 250000.0))) {
        return nullptr;
    }
    return &record;
}

bool legacyFt02ReferenceDeclared(
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
        std::size_t pieceIndex)
{
    using Snapshot = WorkoutGameCourseAssetPhysicsSnapshot;
    if (pieceIndex >= snapshot.pieceBindings.size()) return false;
    const auto &piece = snapshot.pieceBindings[pieceIndex];
    return (piece.flags & Snapshot::LegacyProceduralV1) != 0
            && piece.legacyFt02RecordIndex != Snapshot::NoIndex;
}

}

WorkoutGameAssetPhysicsSample WorkoutGameAssetPhysicsSampler::sample(
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
        std::size_t pieceIndex,
        double distanceMeters)
{
    WorkoutGameAssetPhysicsSample result;
    if (legacyFt02ReferenceDeclared(snapshot, pieceIndex)) {
        result.bound = true;
        const auto *legacy = legacyFt02RecordAt(snapshot, pieceIndex);
        if (!legacy) return result;
        if (!legacy->enabled) return result;
        const double local = distanceMeters - legacy->obstacleAnchorMeters;
        if (local < legacy->startMeters || local > legacy->endMeters) {
            return result;
        }
        result.surfacePresent = true;
        result.offsetMeters = WorkoutGameLegacyFt02V1::surfaceOffsetMeters(
                local, legacy->startMeters, legacy->endMeters,
                legacy->heightMeters);
        return result;
    }
    if (!std::isfinite(distanceMeters)) return result;
    const WorkoutGameAssetPhysicsPieceBinding *piece = nullptr;
    const WorkoutGameAssetPhysicsDefinition *definition =
            definitionAt(snapshot, pieceIndex, piece);
    if (!definition) return result;

    result.bound = true;
    result.obstacleContact = true;
    result.materialDefined = true;
    result.operation = definition->operation;
    result.coulombFriction =
            double(definition->coulombFrictionMilli) / 1000.0;
    result.restitution = double(definition->restitutionMilli) / 1000.0;
    double localMm = distanceMeters * 1000.0
            - piece->obstacleAnchorMeters() * 1000.0;
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
            result.push_back(piece->obstacleAnchorMeters()
                    + double(point.forwardMm) / 1000.0);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<double> WorkoutGameAssetPhysicsSampler::renderBreakpointsMeters(
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
        std::size_t pieceIndex)
{
    if (const auto *legacy = legacyFt02RecordAt(snapshot, pieceIndex)) {
        if (!legacy->enabled) return {};
        std::vector<double> result;
        result.reserve(WorkoutGameLegacyFt02V1::RadialSegments / 2 + 3);
        const auto appendLocal = [&](double local) {
            if (local >= legacy->startMeters
                    && local <= legacy->endMeters) {
                result.push_back(legacy->obstacleAnchorMeters + local);
            }
        };
        appendLocal(legacy->startMeters);
        const double radius = legacy->heightMeters * 0.5;
        constexpr double Pi = 3.14159265358979323846;
        for (int segment = 0;
                segment <= WorkoutGameLegacyFt02V1::RadialSegments / 2;
                ++segment) {
            const double angle = Pi
                    - double(segment) * 2.0 * Pi
                        / double(WorkoutGameLegacyFt02V1::RadialSegments);
            appendLocal(std::cos(angle) * radius);
        }
        appendLocal(legacy->endMeters);
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }
    return breakpointsMeters(snapshot, pieceIndex);
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
    if ((piece.flags & Snapshot::LegacyProceduralV1) != 0) {
        const auto *legacy = legacyFt02RecordAt(snapshot, pieceIndex);
        if (!legacy) {
            if (piece.legacyFt02RecordIndex != Snapshot::NoIndex) {
                result.status = WorkoutGameAssetRenderFitStatus::Invalid;
            }
            return result;
        }
        if (!legacy->enabled) return result;
        result.status = WorkoutGameAssetRenderFitStatus::Ready;
        result.assetId = QStringLiteral("FT-02-log-over-greybox");
        result.exactLegacy = true;
        result.exactObstacleAnchorMeters = legacy->obstacleAnchorMeters;
        result.exactStartMeters = legacy->startMeters;
        result.exactEndMeters = legacy->endMeters;
        result.exactHeightMeters = legacy->heightMeters;
        return result;
    }
    if ((piece.definitionIndex == Snapshot::NoIndex
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
    const bool supportedFt02RenderFit =
            binding.assetId != QStringLiteral("FT-02-log-over-greybox")
            || (binding.variantKey.isEmpty()
                && binding.nativeForwardOriginMm == -1020
                && binding.nativeForwardExtentMm == 540
                && binding.nativeUpExtentMm == 540);
    if (binding.definitionIndex != piece.definitionIndex
            || binding.nativeForwardExtentMm == 0
            || binding.nativeUpExtentMm == 0
            || binding.resolvedExtentMm == 0
            || !supportedFt02RenderFit) {
        result.status = WorkoutGameAssetRenderFitStatus::Invalid;
        return result;
    }
    result.status = WorkoutGameAssetRenderFitStatus::Ready;
    result.assetId = binding.assetId;
    result.variantKey = binding.variantKey;
    result.obstacleAnchorMm = piece.obstacleAnchorMm;
    result.obstacleAnchorMicrometerRemainder =
            piece.obstacleAnchorMicrometerRemainder;
    result.nativeForwardOriginMm = binding.nativeForwardOriginMm;
    result.nativeForwardExtentMm = binding.nativeForwardExtentMm;
    result.nativeUpExtentMm = binding.nativeUpExtentMm;
    result.resolvedExtentMm = binding.resolvedExtentMm;
    return result;
}

WorkoutGameAssetRenderTransform
WorkoutGameAssetPhysicsSampler::renderTransform(
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot,
        std::size_t pieceIndex)
{
    WorkoutGameAssetRenderTransform result;
    const WorkoutGameAssetRenderFit fit = renderFit(snapshot, pieceIndex);
    result.status = fit.status;
    if (fit.status != WorkoutGameAssetRenderFitStatus::Ready) return result;

    result.assetId = fit.assetId;
    result.variantKey = fit.variantKey;
    if (fit.exactLegacy) {
        const double forwardExtent = fit.exactEndMeters
                - fit.exactStartMeters;
        result.obstacleAnchorMeters = fit.exactObstacleAnchorMeters;
        result.obstacleStartDistanceMeters = result.obstacleAnchorMeters
                + fit.exactStartMeters;
        result.obstacleEndDistanceMeters = result.obstacleAnchorMeters
                + fit.exactEndMeters;
        result.forwardScale = forwardExtent / Ft02NativeExtentMeters;
        result.upScale = fit.exactHeightMeters / Ft02NativeExtentMeters;
        result.assetStartDistanceMeters = result.obstacleAnchorMeters
                + fit.exactStartMeters
                - Ft02NativeDeadZoneMeters * result.forwardScale;
        result.forwardExtentMeters = forwardExtent;
        result.upExtentMeters = fit.exactHeightMeters;
        return result;
    }
    result.obstacleAnchorMeters =
            (double(fit.obstacleAnchorMm) * 1000.0
             + fit.obstacleAnchorMicrometerRemainder) / 1000000.0;
    result.obstacleStartDistanceMeters = result.obstacleAnchorMeters
            - double(fit.resolvedExtentMm) / 2000.0;
    result.obstacleEndDistanceMeters = result.obstacleAnchorMeters
            + double(fit.resolvedExtentMm) / 2000.0;
    result.forwardScale = double(fit.resolvedExtentMm)
            / double(fit.nativeForwardExtentMm);
    result.upScale = double(fit.resolvedExtentMm)
            / double(fit.nativeUpExtentMm);
    result.assetStartDistanceMeters = result.obstacleAnchorMeters
            + double(fit.nativeForwardOriginMm) / 1000.0
                * result.forwardScale;
    result.forwardExtentMeters =
            double(fit.nativeForwardExtentMm) / 1000.0
                * result.forwardScale;
    result.upExtentMeters = double(fit.nativeUpExtentMm) / 1000.0
            * result.upScale;
    if (!std::isfinite(result.obstacleAnchorMeters)
            || !std::isfinite(result.obstacleStartDistanceMeters)
            || !std::isfinite(result.obstacleEndDistanceMeters)
            || !std::isfinite(result.assetStartDistanceMeters)
            || !std::isfinite(result.forwardScale)
            || !std::isfinite(result.upScale)
            || !std::isfinite(result.forwardExtentMeters)
            || !std::isfinite(result.upExtentMeters)
            || result.forwardScale <= 0.0 || result.upScale <= 0.0
            || result.obstacleStartDistanceMeters
                >= result.obstacleEndDistanceMeters
            || result.forwardExtentMeters <= 0.0
            || result.upExtentMeters <= 0.0) {
        result = {};
        result.status = WorkoutGameAssetRenderFitStatus::Invalid;
    }
    return result;
}
