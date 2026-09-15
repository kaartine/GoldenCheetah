/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameForestInstancing.h"

#include <QColor>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr double MinimumVisibleOpacityChange = 0.025;
constexpr double OpaqueOpacity = 0.999;

double finiteOrZero(double value)
{
    return std::isfinite(value) ? value : 0.0;
}

}

WorkoutGameForestInstancing::WorkoutGameForestInstancing(
        bool treePresentation, RenderPass renderPass,
        QQuick3DObject *parent) :
    QQuick3DInstancing(parent),
    treePresentation(treePresentation),
    renderPass(renderPass)
{
    const bool transparent = renderPass != RenderPass::Opaque;
    setHasTransparency(transparent);
    setDepthSortingEnabled(transparent);
}

void WorkoutGameForestInstancing::setPlacements(
        const QVariantList &placements)
{
    if (currentPlacements == placements) return;
    currentPlacements = placements;
    rebuildBuffer(true);
}

void WorkoutGameForestInstancing::setPresentation(
        double riderDistanceMeters,
        const QVector3D &cameraPosition,
        const QVector3D &cameraTarget)
{
    const double distance = finiteOrZero(riderDistanceMeters);
    if (presentationReady
            && qFuzzyCompare(currentRiderDistanceMeters, distance)
            && currentCameraPosition == cameraPosition
            && currentCameraTarget == cameraTarget) {
        return;
    }
    currentRiderDistanceMeters = distance;
    currentCameraPosition = cameraPosition;
    currentCameraTarget = cameraTarget;
    presentationReady = true;
    rebuildBuffer();
}

QString WorkoutGameForestInstancing::stableIdAt(int index) const
{
    if (index < 0 || index >= renderedPlacementIndices.size()) return {};
    return currentPlacements.at(renderedPlacementIndices.at(index))
            .toMap().value(
            QStringLiteral("stableId")).toString();
}

double WorkoutGameForestInstancing::opacityAt(int index) const
{
    return index >= 0 && index < renderedPlacementIndices.size()
            ? currentOpacities.at(renderedPlacementIndices.at(index)) : 0.0;
}

QByteArray WorkoutGameForestInstancing::getInstanceBuffer(int *instanceCount)
{
    if (instanceCount) *instanceCount = renderedPlacementIndices.size();
    return currentBuffer;
}

double WorkoutGameForestInstancing::edgeOpacity(
        double relativeDistanceMeters) const
{
    if (treePresentation) {
        const double behind = std::clamp(
                (relativeDistanceMeters + 19.0) / 6.0, 0.0, 1.0);
        const double ahead = std::clamp(
                (34.0 - relativeDistanceMeters) / 8.0, 0.0, 1.0);
        return std::min(behind, ahead);
    }
    const double behind = std::clamp(
            (relativeDistanceMeters + 14.0) / 4.0, 0.0, 1.0);
    const double ahead = std::clamp(
            (44.0 - relativeDistanceMeters) / 8.0, 0.0, 1.0);
    return std::min(behind, ahead);
}

double WorkoutGameForestInstancing::cameraOpacity(
        double x, double z, double radius,
        const QVector3D &cameraPosition,
        const QVector3D &cameraTarget) const
{
    const double segmentX = double(cameraTarget.x() - cameraPosition.x());
    const double segmentZ = double(cameraTarget.z() - cameraPosition.z());
    const double lengthSquared = segmentX * segmentX + segmentZ * segmentZ;
    double blend = 0.0;
    if (lengthSquared > 1.0e-9) {
        blend = std::clamp(
                ((x - cameraPosition.x()) * segmentX
                    + (z - cameraPosition.z()) * segmentZ)
                    / lengthSquared,
                0.0, 1.0);
    }
    const double nearestX = cameraPosition.x() + blend * segmentX;
    const double nearestZ = cameraPosition.z() + blend * segmentZ;
    const double clearance = std::hypot(x - nearestX, z - nearestZ);
    const double opaqueStart = treePresentation ? radius + 0.85 : 0.55;
    const double fadeWidth = treePresentation ? 1.25 : 0.9;
    return std::clamp(
            (clearance - opaqueStart) / fadeWidth, 0.0, 1.0);
}

void WorkoutGameForestInstancing::rebuildBuffer(bool forceUpdate)
{
    QVector<double> nextOpacities;
    nextOpacities.reserve(currentPlacements.size());
    for (const QVariant &placementValue : currentPlacements) {
        const QVariantMap placement = placementValue.toMap();
        nextOpacities.push_back(presentationReady
                ? std::min(
                    edgeOpacity(placement.value(QStringLiteral("distance"))
                                    .toDouble()
                                - currentRiderDistanceMeters),
                    cameraOpacity(
                        placement.value(QStringLiteral("x")).toDouble(),
                        placement.value(QStringLiteral("z")).toDouble(),
                        placement.value(QStringLiteral("crownRadius"))
                                .toDouble(),
                        currentCameraPosition, currentCameraTarget))
                : 1.0);
    }
    bool opacityChanged = currentOpacities.size() != nextOpacities.size();
    for (int index = 0; !opacityChanged && index < nextOpacities.size();
            ++index) {
        const double currentOpacity = currentOpacities.at(index);
        const double nextOpacity = nextOpacities.at(index);
        opacityChanged = std::abs(currentOpacity - nextOpacity)
                        > MinimumVisibleOpacityChange
                || (currentOpacity >= OpaqueOpacity)
                        != (nextOpacity >= OpaqueOpacity);
    }
    if (!forceUpdate && !opacityChanged) return;
    currentOpacities = nextOpacities;
    renderedPlacementIndices.clear();
    renderedPlacementIndices.reserve(currentPlacements.size());
    for (int index = 0; index < currentPlacements.size(); ++index) {
        const double opacity = currentOpacities.at(index);
        const bool include = renderPass == RenderPass::Combined
                || (renderPass == RenderPass::Opaque
                    && opacity >= OpaqueOpacity)
                || (renderPass == RenderPass::Fading
                    && opacity < OpaqueOpacity);
        if (include) renderedPlacementIndices.push_back(index);
    }
    currentBuffer.resize(
            renderedPlacementIndices.size() * int(sizeof(InstanceTableEntry)));

    for (int outputIndex = 0;
            outputIndex < renderedPlacementIndices.size(); ++outputIndex) {
        const int placementIndex = renderedPlacementIndices.at(outputIndex);
        const QVariantMap placement =
                currentPlacements.at(placementIndex).toMap();
        const double opacity = renderPass == RenderPass::Opaque
                ? 1.0 : currentOpacities.at(placementIndex);

        const double scale = placement.value(
                QStringLiteral("scale"), 1.0).toDouble();
        const QVector3D instanceScale(
                float(placement.value(QStringLiteral("mirror")).toBool()
                    ? -scale : scale),
                float(scale), float(scale));
        const InstanceTableEntry entry = calculateTableEntry(
                QVector3D(
                    float(placement.value(QStringLiteral("x")).toDouble()),
                    float(placement.value(QStringLiteral("y")).toDouble()),
                    float(placement.value(QStringLiteral("z")).toDouble())),
                instanceScale,
                QVector3D(
                    float(placement.value(QStringLiteral("pitch")).toDouble()),
                    float(placement.value(QStringLiteral("yaw")).toDouble()),
                    float(placement.value(
                        QStringLiteral("terrainRoll")).toDouble())),
                QColor::fromRgbF(1.0, 1.0, 1.0, opacity));
        std::memcpy(
                currentBuffer.data()
                    + outputIndex * int(sizeof(InstanceTableEntry)),
                &entry, sizeof(InstanceTableEntry));
    }
    markDirty();
}
