/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DGeometry.h"

#include "WorkoutGameBermGeometry.h"
#include "WorkoutGameClimbGeometry.h"
#include "WorkoutGame3DTerrainProfile.h"
#include "WorkoutGameFeatureGeometry.h"
#include "WorkoutGameRockGardenGeometry.h"
#include "WorkoutGameRockSlabGeometry.h"
#include "WorkoutGameRootGeometry.h"
#include "WorkoutGameSkinnyGeometry.h"
#include "WorkoutGameTrailBranch.h"

#include <QByteArray>
#include <QVector3D>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace {

constexpr int MaximumSamples = 16000;
constexpr double MinimumSampleSpacingMeters = 0.75;
constexpr double BypassHalfWidthMeters = 0.38;
constexpr double BypassEdgeWidthMeters = 0.08;
constexpr double ForestDressingSpacingMeters = 3.0;
constexpr double Pi = 3.14159265358979323846;

std::pair<double, double> challengeGeometrySpan(
        const WorkoutGameRoadPiece &piece)
{
    switch (piece.terrain) {
    case WorkoutGameTerrainKind::Roots: {
        const auto profile = WorkoutGameRootGeometry::profile(piece.difficulty);
        return {profile.startMeters, profile.endMeters};
    }
    case WorkoutGameTerrainKind::RockGarden: {
        const auto profile = WorkoutGameRockGardenGeometry::profile(
                piece.difficulty);
        return {profile.startMeters, profile.endMeters};
    }
    case WorkoutGameTerrainKind::RockSlab: {
        const auto profile = WorkoutGameRockSlabGeometry::profile(
                piece.difficulty);
        return {profile.startMeters, profile.endMeters};
    }
    case WorkoutGameTerrainKind::Skinny: {
        const auto profile = WorkoutGameSkinnyGeometry::profile(
                piece.difficulty);
        return {profile.startMeters, profile.endMeters};
    }
    case WorkoutGameTerrainKind::Climb: {
        const auto profile = WorkoutGameClimbGeometry::profile(piece.difficulty);
        return {profile.startMeters, profile.endMeters};
    }
    case WorkoutGameTerrainKind::Berm: {
        const auto profile = WorkoutGameBermGeometry::profile(piece.difficulty);
        return {profile.startMeters, profile.endMeters};
    }
    default: {
        const auto profile = WorkoutGameFeatureGeometry::profile(
                piece.terrain, piece.difficulty);
        return profile.ready
                ? std::pair<double, double>{
                    profile.startMeters, profile.endMeters}
                : std::pair<double, double>{0.0, 0.0};
    }
    }
}

bool overlapsChallengeCorridor(
        const WorkoutGameRoadCourse &course,
        double distanceMeters)
{
    constexpr double DressingClearanceMeters = 2.0;
    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (!piece.challenge.enabled) continue;
        const auto [featureStart, featureEnd] = challengeGeometrySpan(piece);
        const double start = std::min({
                piece.challenge.prepareDistanceMeters,
                piece.challenge.bypassStartDistanceMeters,
                piece.challenge.obstacleDistanceMeters + featureStart})
                - DressingClearanceMeters;
        const double end = std::max({
                piece.challenge.bypassEndDistanceMeters,
                piece.challenge.obstacleDistanceMeters,
                piece.challenge.obstacleDistanceMeters + featureEnd})
                + DressingClearanceMeters;
        if (distanceMeters >= start && distanceMeters <= end) return true;
    }
    return false;
}

struct Vertex
{
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    float r;
    float g;
    float b;
    float a;
    float u;
    float v;
};

struct TerrainColor
{
    float r;
    float g;
    float b;
};

TerrainColor colorFor(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::Roots: return {0.43f, 0.27f, 0.13f};
    case WorkoutGameTerrainKind::RockGarden: return {0.45f, 0.42f, 0.35f};
    case WorkoutGameTerrainKind::Skinny: return {0.48f, 0.31f, 0.15f};
    case WorkoutGameTerrainKind::RockSlab: return {0.50f, 0.32f, 0.16f};
    case WorkoutGameTerrainKind::Climb: return {0.49f, 0.31f, 0.15f};
    default: return {0.50f, 0.32f, 0.16f};
    }
}

std::uint32_t forestHash(std::uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

void appendBytes(QByteArray &data, const void *source, std::size_t size)
{
    data.append(static_cast<const char *>(source), qsizetype(size));
}

WorkoutGame3DMeshData meshData(
        const std::vector<Vertex> &vertices,
        const std::vector<std::uint32_t> &indices,
        const QVector3D &boundsMin,
        const QVector3D &boundsMax,
        int sampleCount)
{
    if (vertices.empty() || indices.empty() || sampleCount <= 0) return {};
    WorkoutGame3DMeshData result;
    result.vertexData.reserve(
            qsizetype(vertices.size() * sizeof(Vertex)));
    appendBytes(result.vertexData, vertices.data(),
                vertices.size() * sizeof(Vertex));
    result.indexData.reserve(
            qsizetype(indices.size() * sizeof(std::uint32_t)));
    appendBytes(result.indexData, indices.data(),
                indices.size() * sizeof(std::uint32_t));
    result.boundsMin = boundsMin;
    result.boundsMax = boundsMax;
    result.sampleCount = sampleCount;
    result.ready = true;
    return result;
}

void appendFeatureSamples(
        const WorkoutGameRoadCourse &course,
        double startDistanceMeters,
        double endDistanceMeters,
        std::vector<double> &distances)
{
    const auto append = [&](double distance) {
        if (distance >= startDistanceMeters
                && distance <= endDistanceMeters
                && std::isfinite(distance)) {
            distances.push_back(distance);
        }
    };
    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (!piece.challenge.enabled) continue;
        if (piece.terrain == WorkoutGameTerrainKind::RockSlab) {
            const WorkoutGameRockSlabGeometryProfile slab =
                    WorkoutGameRockSlabGeometry::profile(
                        piece.difficulty);
            const double center = piece.challenge.obstacleDistanceMeters;
            append(center + slab.startMeters);
            append(center + slab.activeStartMeters);
            append(center + slab.crestMeters);
            append(center + slab.activeEndMeters);
            append(center + slab.endMeters);
            continue;
        }
        if (piece.terrain == WorkoutGameTerrainKind::Skinny) {
            const WorkoutGameSkinnyGeometryProfile skinny =
                    WorkoutGameSkinnyGeometry::profile(piece.difficulty);
            const double center = piece.challenge.obstacleDistanceMeters;
            append(center + skinny.startMeters);
            append(center + skinny.activeStartMeters);
            append(center + skinny.deckStartMeters);
            append(center + skinny.deckEndMeters);
            append(center + skinny.activeEndMeters);
            append(center + skinny.endMeters);
            continue;
        }
        if (piece.terrain == WorkoutGameTerrainKind::Climb) {
            const WorkoutGameClimbGeometryProfile climb =
                    WorkoutGameClimbGeometry::profile(piece.difficulty);
            const double center = piece.challenge.obstacleDistanceMeters;
            append(center + climb.startMeters);
            append(center + climb.activeStartMeters);
            for (const WorkoutGameClimbStep &step : climb.steps) {
                append(center + step.forwardMeters
                        - step.halfLengthMeters - climb.contactRampMeters);
                append(center + step.forwardMeters - step.halfLengthMeters);
                append(center + step.forwardMeters);
                append(center + step.forwardMeters + step.halfLengthMeters);
                append(center + step.forwardMeters
                        + step.halfLengthMeters + climb.contactRampMeters);
            }
            append(center + climb.crestStartMeters);
            append(center + climb.endMeters);
            continue;
        }
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece.terrain, piece.difficulty);
        if (!profile.ready) continue;
        const double center = piece.challenge.obstacleDistanceMeters;
        append(center + profile.startMeters);
        append(center + profile.plateauStartMeters);
        append(center);
        append(center + profile.plateauEndMeters);
        append(center + profile.endMeters);
        if (profile.shape == WorkoutGameFeatureGeometryShape::DropLedge) {
            append(center + profile.landingStartMeters);
            append(center + profile.recoveryStartMeters);
        }
        if (profile.shape
                == WorkoutGameFeatureGeometryShape::RoundedRollers) {
            constexpr int RollerHalfWaves = 6;
            for (int halfWave = 0;
                    halfWave <= RollerHalfWaves; ++halfWave) {
                append(center + profile.plateauStartMeters
                        + (profile.plateauEndMeters
                           - profile.plateauStartMeters)
                            * double(halfWave)
                            / double(RollerHalfWaves));
            }
        }
        if (profile.shape == WorkoutGameFeatureGeometryShape::FacetedLog) {
            const double radius = profile.heightMeters * 0.5;
            for (int segment = 0;
                 segment <= WorkoutGameLogRadialSegments / 2; ++segment) {
                const double angle = Pi
                        - double(segment) * 2.0 * Pi
                            / double(WorkoutGameLogRadialSegments);
                append(center + std::cos(angle) * radius);
            }
        }
    }
    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (piece.terrain != WorkoutGameTerrainKind::Berm) {
            continue;
        }
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(piece.difficulty);
        const double center = piece.geometryAnchorDistanceMeters;
        const auto append = [&](double distance) {
            if (distance >= startDistanceMeters
                    && distance <= endDistanceMeters) {
                distances.push_back(distance);
            }
        };
        append(center + profile.startMeters);
        constexpr int CurveSegments = 16;
        for (int segment = 0; segment <= CurveSegments; ++segment) {
            append(center + profile.curveStartMeters
                    + (profile.curveEndMeters - profile.curveStartMeters)
                        * double(segment) / double(CurveSegments));
        }
        append(center + profile.endMeters);
    }
}

}

WorkoutGame3DGeometry::WorkoutGame3DGeometry(
        Layer layer,
        QQuick3DObject *parent) :
    QQuick3DGeometry(parent),
    layer(layer)
{
}

void WorkoutGame3DGeometry::setCourse(const WorkoutGameRoadCourse &course)
{
    const double endDistanceMeters = std::max(
            course.totalLengthMeters, course.visualLengthMeters);
    setMeshData(buildMeshData(
            layer, course, 0.0, endDistanceMeters));
}

void WorkoutGame3DGeometry::setCourseRange(
        const WorkoutGameRoadCourse &course,
        double startDistanceMeters,
        double endDistanceMeters)
{
    setMeshData(buildMeshData(
            layer, course, startDistanceMeters, endDistanceMeters));
}

void WorkoutGame3DGeometry::setMeshData(
        const WorkoutGame3DMeshData &data)
{
    clear();
    geometryReady = false;
    generatedSampleCount = 0;
    if (!data.ready) {
        update();
        return;
    }
    setStride(sizeof(Vertex));
    setVertexData(data.vertexData);
    setIndexData(data.indexData);
    setPrimitiveType(PrimitiveType::Triangles);
    addAttribute(Attribute::PositionSemantic,
                 offsetof(Vertex, x), Attribute::F32Type);
    addAttribute(Attribute::NormalSemantic,
                 offsetof(Vertex, nx), Attribute::F32Type);
    addAttribute(Attribute::ColorSemantic,
                 offsetof(Vertex, r), Attribute::F32Type);
    addAttribute(Attribute::TexCoordSemantic,
                 offsetof(Vertex, u), Attribute::F32Type);
    addAttribute(Attribute::IndexSemantic, 0, Attribute::U32Type);
    setBounds(data.boundsMin, data.boundsMax);
    geometryReady = true;
    generatedSampleCount = data.sampleCount;
    update();
}

WorkoutGame3DMeshData WorkoutGame3DGeometry::buildMeshData(
        Layer layer,
        const WorkoutGameRoadCourse &course,
        double requestedStartDistanceMeters,
        double requestedEndDistanceMeters)
{
    const double visualLength = std::max(
            course.totalLengthMeters, course.visualLengthMeters);
    if (!course.ready || visualLength <= 0.0) return {};

    const double startDistanceMeters = std::clamp(
            requestedStartDistanceMeters, 0.0, visualLength);
    const double endDistanceMeters = std::clamp(
            requestedEndDistanceMeters, 0.0, visualLength);
    if (!std::isfinite(startDistanceMeters)
            || !std::isfinite(endDistanceMeters)
            || endDistanceMeters <= startDistanceMeters) {
        return {};
    }
    if (layer == Layer::Bypass) {
        return buildBypasses(course, startDistanceMeters, endDistanceMeters);
    }
    if (layer == Layer::Berm) {
        return buildBerms(course, startDistanceMeters, endDistanceMeters);
    }
    if (layer == Layer::Climb) {
        return buildClimbs(course, startDistanceMeters, endDistanceMeters);
    }
    if (layer == Layer::ForestDressing) {
        return buildForestDressing(
                course, startDistanceMeters, endDistanceMeters);
    }
    if (layer == Layer::Roots) {
        return buildRoots(course, startDistanceMeters, endDistanceMeters);
    }
    if (layer == Layer::RockGarden) {
        return buildRockGardens(
                course, startDistanceMeters, endDistanceMeters);
    }
    if (layer == Layer::RockSlab) {
        return buildRockSlabs(course, startDistanceMeters, endDistanceMeters);
    }
    if (layer == Layer::Skinny) {
        return buildSkinnies(course, startDistanceMeters, endDistanceMeters);
    }
    const double rangeMeters = endDistanceMeters - startDistanceMeters;

    std::vector<double> sampleDistances;
    appendFeatureSamples(
            course, startDistanceMeters, endDistanceMeters, sampleDistances);
    std::sort(sampleDistances.begin(), sampleDistances.end());
    sampleDistances.erase(std::unique(
            sampleDistances.begin(), sampleDistances.end(),
            [](double left, double right) {
                return std::abs(left - right) < 1e-6;
            }), sampleDistances.end());
    if (sampleDistances.size() > std::size_t(MaximumSamples - 2)) {
        sampleDistances.resize(std::size_t(MaximumSamples - 2));
    }
    const int uniformBudget = std::max(
            2, MaximumSamples - int(sampleDistances.size()));
    const double spacing = std::max(
            MinimumSampleSpacingMeters,
            rangeMeters / double(uniformBudget - 1));
    const int uniformCount = std::clamp(
            int(std::ceil(rangeMeters / spacing)) + 1,
            2, uniformBudget);
    for (int index = 0; index < uniformCount; ++index) {
        sampleDistances.push_back(index + 1 == uniformCount
                ? endDistanceMeters
                : std::min(endDistanceMeters,
                    startDistanceMeters + double(index) * spacing));
    }
    std::sort(sampleDistances.begin(), sampleDistances.end());
    sampleDistances.erase(std::unique(
            sampleDistances.begin(), sampleDistances.end(),
            [](double left, double right) {
                return std::abs(left - right) < 1e-6;
            }), sampleDistances.end());
    const int count = int(sampleDistances.size());
    const int verticesPerSample = layer == Layer::Trail
            ? 2 : int(WorkoutGame3DTerrainProfileSnapshot::VertexCount);
    const int stripsPerSample = verticesPerSample - 1;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<bool> rideableSamples;
    std::vector<bool> renderableTrailSamples;
    std::vector<bool> trailBackingSamples;
    std::vector<bool> floorUnderFeatureSamples;
    vertices.reserve(std::size_t(count * verticesPerSample));
    rideableSamples.reserve(std::size_t(count));
    renderableTrailSamples.reserve(std::size_t(count));
    trailBackingSamples.reserve(std::size_t(count));
    floorUnderFeatureSamples.reserve(std::size_t(count));
    indices.reserve(std::size_t(
            (count - 1) * stripsPerSample * 6));

    QVector3D boundsMin(
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max());
    QVector3D boundsMax(
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest());

    for (int index = 0; index < count; ++index) {
        const double distance = sampleDistances[std::size_t(index)];
        const WorkoutGameRoadSample sample =
                WorkoutGameRoadCourseBuilder::sampleVisual(course, distance);
        if (!sample.ready) {
            return {};
        }
        rideableSamples.push_back(sample.rideableSurface);
        renderableTrailSamples.push_back(sample.renderableTrailSurface);
        trailBackingSamples.push_back(sample.renderableTrailSurface
                || sample.terrain == WorkoutGameTerrainKind::Berm);
        floorUnderFeatureSamples.push_back(
                sample.terrain == WorkoutGameTerrainKind::Skinny);
        const double rightX = std::cos(sample.center.headingRadians);
        const double rightZ = -std::sin(sample.center.headingRadians);
        const TerrainColor color = layer == Layer::Trail
                ? colorFor(sample.terrain)
                : TerrainColor{0.20f, 0.32f, 0.17f};
        const WorkoutGame3DTerrainProfileSnapshot terrain =
                layer == Layer::ForestFloor
                ? WorkoutGame3DTerrainProfile::build(
                    sample, distance, course.seed)
                : WorkoutGame3DTerrainProfileSnapshot();
        if (layer == Layer::ForestFloor && !terrain.ready) {
            return {};
        }
        double trailBackingDropMeters = 0.0;
        if (layer == Layer::Trail
                && sample.terrain == WorkoutGameTerrainKind::Berm
                && !sample.renderableTrailSurface
                && sample.pieceIndex < course.pieces.size()) {
            const WorkoutGameRoadPiece &piece =
                    course.pieces[sample.pieceIndex];
            const WorkoutGameBermGeometryProfile berm =
                    WorkoutGameBermGeometry::profile(piece.difficulty);
            const double local = distance
                    - piece.geometryAnchorDistanceMeters;
            const double progress = std::clamp(
                    (local - berm.startMeters)
                        / (berm.endMeters - berm.startMeters),
                    0.0, 1.0);
            const double socketEnvelope = std::pow(
                    std::sin(Pi * progress), 2.0);
            const double bankDepth = 2.0 / 3.0
                    * sample.center.halfWidthMeters
                    * std::tan(std::abs(sample.bermBankRadians));
            trailBackingDropMeters = bankDepth
                    + 0.04 * socketEnvelope;
        }
        for (int vertex = 0; vertex < verticesPerSample; ++vertex) {
            const bool trailVertex = layer == Layer::Trail;
            const double lateral = trailVertex
                    ? (vertex == 0
                        ? -sample.center.halfWidthMeters
                        : sample.center.halfWidthMeters)
                    : terrain.vertices[std::size_t(vertex)].lateralMeters;
            const bool technicalDatum = trailVertex
                    && (sample.terrain == WorkoutGameTerrainKind::Roots
                        || sample.terrain
                            == WorkoutGameTerrainKind::RockGarden
                        || sample.terrain
                            == WorkoutGameTerrainKind::RockSlab
                        || sample.terrain
                            == WorkoutGameTerrainKind::Skinny
                        || sample.terrain
                            == WorkoutGameTerrainKind::Climb);
            const double elevation = trailVertex
                    ? sample.visualGroundElevationMeters()
                        - (technicalDatum ? sample.surfaceOffsetMeters : 0.0)
                        - trailBackingDropMeters
                        + 0.015
                    : terrain.vertices[std::size_t(vertex)].elevationMeters;
            const int previousVertex = std::max(0, vertex - 1);
            const int nextVertex = std::min(verticesPerSample - 1, vertex + 1);
            const double lateralSpan = trailVertex ? 1.0
                    : terrain.vertices[std::size_t(nextVertex)].lateralMeters
                        - terrain.vertices[std::size_t(previousVertex)]
                            .lateralMeters;
            const double crossSlope = trailVertex
                    ? 0.0
                    : (terrain.vertices[std::size_t(nextVertex)].elevationMeters
                        - terrain.vertices[std::size_t(previousVertex)]
                            .elevationMeters)
                        / std::max(1e-6, lateralSpan);
            const bool nearTrail = trailVertex
                    || (!trailVertex && vertex >= 2 && vertex <= 5);
            const double grade = (nearTrail
                    ? sample.center.gradePercent
                    : sample.baseGradePercent) / 100.0;
            const QVector3D forward(
                    float(std::sin(sample.center.headingRadians)),
                    float(grade),
                    float(std::cos(sample.center.headingRadians)));
            const QVector3D right(
                    float(std::cos(sample.center.headingRadians)),
                    float(crossSlope),
                    float(-std::sin(sample.center.headingRadians)));
            QVector3D normal = QVector3D::crossProduct(forward, right);
            normal.normalize();
            const TerrainColor vertexColor = trailVertex
                    ? color
                    : TerrainColor{
                        terrain.vertices[std::size_t(vertex)].red,
                        terrain.vertices[std::size_t(vertex)].green,
                        terrain.vertices[std::size_t(vertex)].blue};
            const float x = float(sample.center.xMeters
                    + lateral * rightX);
            const float y = float(elevation);
            const float z = float(sample.center.zMeters
                    + lateral * rightZ);
            vertices.push_back({
                x, y, z,
                normal.x(), normal.y(), normal.z(),
                vertexColor.r, vertexColor.g, vertexColor.b, 1.0f,
                verticesPerSample > 1
                    ? float(vertex) / float(verticesPerSample - 1)
                    : 0.0f,
                float(distance * 0.22)
            });
            boundsMin.setX(std::min(boundsMin.x(), x));
            boundsMin.setY(std::min(boundsMin.y(), y));
            boundsMin.setZ(std::min(boundsMin.z(), z));
            boundsMax.setX(std::max(boundsMax.x(), x));
            boundsMax.setY(std::max(boundsMax.y(), y));
            boundsMax.setZ(std::max(boundsMax.z(), z));
        }
        if (index > 0 && (layer != Layer::Trail
                || (rideableSamples[std::size_t(index - 1)]
                    && rideableSamples[std::size_t(index)]
                    && trailBackingSamples[std::size_t(index - 1)]
                    && trailBackingSamples[std::size_t(index)]))) {
            const std::uint32_t base = std::uint32_t(
                    index * verticesPerSample);
            for (int strip = 0; strip < stripsPerSample; ++strip) {
                if (layer == Layer::ForestFloor && strip == 3
                        && ((!renderableTrailSamples[std::size_t(index - 1)]
                                && !floorUnderFeatureSamples[
                                    std::size_t(index - 1)])
                            || (!renderableTrailSamples[std::size_t(index)]
                                && !floorUnderFeatureSamples[
                                    std::size_t(index)]))) {
                    continue;
                }
                const std::uint32_t left = std::uint32_t(strip);
                indices.insert(indices.end(), {
                    base - std::uint32_t(verticesPerSample) + left,
                    base + left,
                    base - std::uint32_t(verticesPerSample) + left + 1u,
                    base - std::uint32_t(verticesPerSample) + left + 1u,
                    base + left,
                    base + left + 1u
                });
            }
        }
    }

    return meshData(vertices, indices, boundsMin, boundsMax, count);
}

WorkoutGame3DMeshData WorkoutGame3DGeometry::buildForestDressing(
        const WorkoutGameRoadCourse &course,
        double startDistanceMeters,
        double endDistanceMeters)
{
    constexpr int RowsPerSide = 2;
    constexpr int VerticesPerTree = 20;
    constexpr int TrianglesPerTree = 8;
    constexpr int MaximumVerticesPerProp = 18;
    constexpr int MaximumTrianglesPerProp = 18;
    constexpr double EdgeInsetMeters = 1.5;
    const int firstSlot = int(std::ceil(
            (startDistanceMeters + EdgeInsetMeters)
            / ForestDressingSpacingMeters));
    const int lastSlot = int(std::floor(
            (endDistanceMeters - EdgeInsetMeters)
            / ForestDressingSpacingMeters));
    if (lastSlot < firstSlot) return {};

    const int maximumTrees = std::max(
            0, (lastSlot - firstSlot + 1) * 2 * RowsPerSide);
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(std::size_t(maximumTrees * VerticesPerTree));
    indices.reserve(std::size_t(maximumTrees * TrianglesPerTree * 3));
    QVector3D boundsMin(
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max());
    QVector3D boundsMax(
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest());
    int treeCount = 0;
    int propCount = 0;

    const auto appendVertex = [&](double x, double y, double z,
                                  const QVector3D &normal,
                                  const TerrainColor &color) {
        const float px = float(x);
        const float py = float(y);
        const float pz = float(z);
        vertices.push_back({
            px, py, pz,
            normal.x(), normal.y(), normal.z(),
            color.r, color.g, color.b, 1.0f,
            0.0f, 0.0f
        });
        boundsMin.setX(std::min(boundsMin.x(), px));
        boundsMin.setY(std::min(boundsMin.y(), py));
        boundsMin.setZ(std::min(boundsMin.z(), pz));
        boundsMax.setX(std::max(boundsMax.x(), px));
        boundsMax.setY(std::max(boundsMax.y(), py));
        boundsMax.setZ(std::max(boundsMax.z(), pz));
    };
    const auto appendCard = [&](double x, double y, double z,
                                double widthX, double widthZ,
                                double scale,
                                const TerrainColor &trunkColor,
                                const TerrainColor &crownColor) {
        QVector3D normal(float(-widthZ), 0.0f, float(widthX));
        normal.normalize();
        const double trunkHalfWidth = 0.13 * scale;
        const double trunkHeight = 1.25 * scale;
        const double crownHalfWidth = 1.20 * scale;
        const double crownBase = 0.72 * scale;
        const double crownShoulder = 2.05 * scale;
        const double crownHeight = 4.85 * scale;
        const std::uint32_t base = std::uint32_t(vertices.size());
        appendVertex(x - widthX * trunkHalfWidth, y,
                     z - widthZ * trunkHalfWidth, normal, trunkColor);
        appendVertex(x + widthX * trunkHalfWidth, y,
                     z + widthZ * trunkHalfWidth, normal, trunkColor);
        appendVertex(x + widthX * trunkHalfWidth, y + trunkHeight,
                     z + widthZ * trunkHalfWidth, normal, trunkColor);
        appendVertex(x - widthX * trunkHalfWidth, y + trunkHeight,
                     z - widthZ * trunkHalfWidth, normal, trunkColor);
        indices.insert(indices.end(), {
            base, base + 1u, base + 2u,
            base, base + 2u, base + 3u
        });

        const std::uint32_t lower = std::uint32_t(vertices.size());
        appendVertex(x - widthX * crownHalfWidth, y + crownBase,
                     z - widthZ * crownHalfWidth, normal, crownColor);
        appendVertex(x + widthX * crownHalfWidth, y + crownBase,
                     z + widthZ * crownHalfWidth, normal, crownColor);
        appendVertex(x, y + crownHeight, z, normal, crownColor);
        indices.insert(indices.end(), {lower, lower + 1u, lower + 2u});

        const std::uint32_t upper = std::uint32_t(vertices.size());
        appendVertex(x - widthX * crownHalfWidth * 0.72,
                     y + crownShoulder,
                     z - widthZ * crownHalfWidth * 0.72,
                     normal, crownColor);
        appendVertex(x + widthX * crownHalfWidth * 0.72,
                     y + crownShoulder,
                     z + widthZ * crownHalfWidth * 0.72,
                     normal, crownColor);
        appendVertex(x, y + crownHeight + 0.28 * scale,
                     z, normal, crownColor);
        indices.insert(indices.end(), {upper, upper + 1u, upper + 2u});
    };
    const auto appendRock = [&](double x, double y, double z,
                                double rightX, double rightZ,
                                double forwardX, double forwardZ,
                                double scale) {
        const TerrainColor rockColor{0.34f, 0.35f, 0.31f};
        const QVector3D up(0.0f, 1.0f, 0.0f);
        const double halfRight = 0.34 * scale;
        const double halfForward = 0.28 * scale;
        const std::uint32_t base = std::uint32_t(vertices.size());
        for (const auto &corner : std::array<std::array<double, 2>, 4>{
                 std::array<double, 2>{-1.0, -1.0},
                 std::array<double, 2>{1.0, -1.0},
                 std::array<double, 2>{1.0, 1.0},
                 std::array<double, 2>{-1.0, 1.0}}) {
            appendVertex(
                x + corner[0] * halfRight * rightX
                    + corner[1] * halfForward * forwardX,
                y,
                z + corner[0] * halfRight * rightZ
                    + corner[1] * halfForward * forwardZ,
                up, rockColor);
        }
        appendVertex(x - 0.06 * scale * rightX,
                     y + 0.34 * scale,
                     z - 0.06 * scale * rightZ,
                     up, rockColor);
        indices.insert(indices.end(), {
            base, base + 1u, base + 4u,
            base + 1u, base + 2u, base + 4u,
            base + 2u, base + 3u, base + 4u,
            base + 3u, base, base + 4u,
            base, base + 3u, base + 2u,
            base, base + 2u, base + 1u
        });
    };
    const auto appendStump = [&](double x, double y, double z,
                                 double scale) {
        const TerrainColor stumpColor{0.29f, 0.17f, 0.08f};
        constexpr int Sides = 6;
        const double radius = 0.22 * scale;
        const double height = 0.42 * scale;
        const std::uint32_t base = std::uint32_t(vertices.size());
        for (int ring = 0; ring < 2; ++ring) {
            for (int side = 0; side < Sides; ++side) {
                const double angle = 2.0 * Pi * double(side) / double(Sides);
                const QVector3D normal(float(std::cos(angle)), 0.0f,
                                       float(std::sin(angle)));
                appendVertex(x + radius * std::cos(angle),
                             y + double(ring) * height,
                             z + radius * std::sin(angle),
                             normal, stumpColor);
            }
        }
        const std::uint32_t top = std::uint32_t(vertices.size());
        appendVertex(x, y + height, z, QVector3D(0.0f, 1.0f, 0.0f),
                     stumpColor);
        for (int side = 0; side < Sides; ++side) {
            const std::uint32_t next = std::uint32_t((side + 1) % Sides);
            const std::uint32_t lower = base + std::uint32_t(side);
            const std::uint32_t upper = lower + Sides;
            indices.insert(indices.end(), {
                lower, base + next, upper + next,
                lower, upper + next, upper,
                upper, upper + next, top
            });
        }
    };
    const auto appendShrub = [&](double x, double y, double z,
                                 double rightX, double rightZ,
                                 double forwardX, double forwardZ,
                                 double scale) {
        const TerrainColor shrubColor{0.09f, 0.29f, 0.12f};
        const auto appendShrubCard = [&](double axisX, double axisZ) {
            QVector3D normal(float(-axisZ), 0.0f, float(axisX));
            normal.normalize();
            const double halfWidth = 0.46 * scale;
            const double height = 0.72 * scale;
            const std::uint32_t base = std::uint32_t(vertices.size());
            appendVertex(x - axisX * halfWidth, y,
                         z - axisZ * halfWidth, normal, shrubColor);
            appendVertex(x + axisX * halfWidth, y,
                         z + axisZ * halfWidth, normal, shrubColor);
            appendVertex(x + axisX * halfWidth * 0.55, y + height * 0.66,
                         z + axisZ * halfWidth * 0.55, normal, shrubColor);
            appendVertex(x, y + height, z, normal, shrubColor);
            appendVertex(x - axisX * halfWidth * 0.55, y + height * 0.66,
                         z - axisZ * halfWidth * 0.55, normal, shrubColor);
            indices.insert(indices.end(), {
                base, base + 1u, base + 2u,
                base, base + 2u, base + 3u,
                base, base + 3u, base + 4u
            });
        };
        appendShrubCard(rightX, rightZ);
        appendShrubCard(forwardX, forwardZ);
    };

    vertices.reserve(vertices.capacity()
            + std::size_t(maximumTrees / RowsPerSide)
                * MaximumVerticesPerProp);
    indices.reserve(indices.capacity()
            + std::size_t(maximumTrees / RowsPerSide)
                * MaximumTrianglesPerProp * 3u);

    for (int slot = firstSlot; slot <= lastSlot; ++slot) {
        for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
            const double side = sideIndex == 0 ? -1.0 : 1.0;
            for (int row = 0; row < RowsPerSide; ++row) {
                const std::uint32_t random = forestHash(
                        course.seed
                        ^ std::uint32_t(slot * 0x9e3779b9u)
                        ^ std::uint32_t((sideIndex * RowsPerSide + row + 1)
                                        * 0x85ebca6bu));
                const double distance = std::clamp(
                        double(slot) * ForestDressingSpacingMeters
                        + (row == 0 ? -0.7 : 0.9)
                        + (double((random >> 8) & 255u) / 255.0 - 0.5)
                            * 0.7,
                        startDistanceMeters + EdgeInsetMeters,
                        endDistanceMeters - EdgeInsetMeters);
                const WorkoutGameRoadSample sample =
                        WorkoutGameRoadCourseBuilder::sampleVisual(course, distance);
                if (!sample.ready) continue;
                const double lateral = side * (
                        (row == 0 ? 4.8 : 8.4)
                        + double((random >> 16) & 255u) / 255.0
                            * (row == 0 ? 1.5 : 2.6));
                const WorkoutGame3DTerrainProfileSnapshot terrain =
                        WorkoutGame3DTerrainProfile::build(
                            sample, distance, course.seed);
                if (!terrain.ready) continue;
                const double rightX = std::cos(sample.center.headingRadians);
                const double rightZ = -std::sin(sample.center.headingRadians);
                const double forwardX = std::sin(sample.center.headingRadians);
                const double forwardZ = std::cos(sample.center.headingRadians);
                const double x = sample.center.xMeters + lateral * rightX;
                const double z = sample.center.zMeters + lateral * rightZ;
                const double y = WorkoutGame3DTerrainProfile::elevationAtLateral(
                        terrain, lateral);
                const double scale = 0.74
                        + double(random & 255u) / 255.0 * 0.42;
                const float shade = float((random >> 24) & 3u) * 0.018f;
                const TerrainColor trunkColor{0.22f, 0.13f, 0.07f};
                const TerrainColor crownColor{
                    0.055f + shade * 0.30f,
                    0.19f + shade,
                    0.095f + shade * 0.45f
                };
                appendCard(x, y, z, rightX, rightZ, scale,
                           trunkColor, crownColor);
                appendCard(x, y, z, forwardX, forwardZ, scale,
                           trunkColor, crownColor);
                ++treeCount;

                if (row != 0) continue;
                const std::uint32_t propRandom = forestHash(
                        random ^ 0xa24baed5u);
                const double propLateral = side * (
                        1.65 + double((propRandom >> 8) & 255u) / 255.0
                            * 1.65);
                const double propDistance = std::clamp(
                        distance
                            + (double(propRandom & 255u) / 255.0 - 0.5)
                                * 2.1,
                        startDistanceMeters + EdgeInsetMeters,
                        endDistanceMeters - EdgeInsetMeters);
                if (overlapsChallengeCorridor(course, propDistance)) continue;
                const WorkoutGameRoadSample propSample =
                        WorkoutGameRoadCourseBuilder::sampleVisual(
                            course, propDistance);
                if (!propSample.ready) continue;
                const WorkoutGame3DTerrainProfileSnapshot propTerrain =
                        WorkoutGame3DTerrainProfile::build(
                            propSample, propDistance, course.seed);
                if (!propTerrain.ready) continue;
                const double propRightX =
                        std::cos(propSample.center.headingRadians);
                const double propRightZ =
                        -std::sin(propSample.center.headingRadians);
                const double propForwardX =
                        std::sin(propSample.center.headingRadians);
                const double propForwardZ =
                        std::cos(propSample.center.headingRadians);
                const double propX = propSample.center.xMeters
                        + propLateral * propRightX;
                const double propZ = propSample.center.zMeters
                        + propLateral * propRightZ;
                const double propY =
                        WorkoutGame3DTerrainProfile::elevationAtLateral(
                            propTerrain, propLateral);
                const double propScale = 0.72
                        + double((propRandom >> 16) & 255u) / 255.0 * 0.56;
                switch ((propRandom >> 24) % 3u) {
                case 0u:
                    appendRock(propX, propY - 0.04 * propScale, propZ,
                               propRightX, propRightZ,
                               propForwardX, propForwardZ, propScale);
                    break;
                case 1u:
                    appendStump(propX, propY, propZ, propScale);
                    break;
                default:
                    appendShrub(propX, propY, propZ,
                                propRightX, propRightZ,
                                propForwardX, propForwardZ, propScale);
                    break;
                }
                ++propCount;
            }
        }
    }

    return meshData(vertices, indices, boundsMin, boundsMax,
                    treeCount + propCount);
}

WorkoutGame3DMeshData WorkoutGame3DGeometry::buildClimbs(
        const WorkoutGameRoadCourse &course,
        double startDistanceMeters,
        double endDistanceMeters)
{
    struct LocalVertex
    {
        double forward;
        double lateral;
        double up;
    };
    constexpr int RampSegments = 4;
    constexpr int VerticesPerStep = 136;
    constexpr int TrianglesPerStep = 68;
    constexpr int MaximumSteps = 120;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(MaximumSteps * VerticesPerStep);
    indices.reserve(MaximumSteps * TrianglesPerStep * 3);
    QVector3D boundsMin(
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max());
    QVector3D boundsMax(
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest());
    int stepCount = 0;

    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (!piece.challenge.enabled
                || piece.terrain != WorkoutGameTerrainKind::Climb) {
            continue;
        }
        const WorkoutGameClimbGeometryProfile profile =
                WorkoutGameClimbGeometry::profile(piece.difficulty);
        const double anchor = piece.challenge.obstacleDistanceMeters;
        if (!profile.ready || anchor + profile.endMeters < startDistanceMeters
                || anchor + profile.startMeters > endDistanceMeters) {
            continue;
        }

        for (const WorkoutGameClimbStep &step : profile.steps) {
            if (stepCount >= MaximumSteps) break;
            const double distance = anchor + step.forwardMeters;
            const double yaw = step.yawDegrees * Pi / 180.0;
            const double cosine = std::cos(yaw);
            const double sine = std::sin(yaw);
            const double longitudinalExtent =
                    std::abs(cosine) * (step.halfLengthMeters
                        + profile.contactRampMeters)
                    + std::abs(sine) * step.halfWidthMeters;
            if (distance + longitudinalExtent < startDistanceMeters
                    || distance - longitudinalExtent > endDistanceMeters) {
                continue;
            }
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sampleVisual(course, distance);
            if (!sample.ready) continue;

            const double halfHeight = step.heightMeters * 0.625;
            const double centerUp = step.heightMeters * 0.375 + 0.006;
            const double low = centerUp - halfHeight;
            const double high = centerUp + halfHeight;
            const double f = step.halfLengthMeters;
            const double w = step.halfWidthMeters;
            const double forwardX = std::sin(sample.center.headingRadians);
            const double forwardZ = std::cos(sample.center.headingRadians);
            const double rightX = std::cos(sample.center.headingRadians);
            const double rightZ = -std::sin(sample.center.headingRadians);
            const double grade = sample.baseGradePercent / 100.0;
            const auto appendFace = [&](const std::array<LocalVertex, 4> &face) {
                const LocalVertex firstEdge{
                    face[1].forward - face[0].forward,
                    face[1].lateral - face[0].lateral,
                    face[1].up - face[0].up};
                const LocalVertex secondEdge{
                    face[2].forward - face[0].forward,
                    face[2].lateral - face[0].lateral,
                    face[2].up - face[0].up};
                LocalVertex localNormal{
                    firstEdge.lateral * secondEdge.up
                        - firstEdge.up * secondEdge.lateral,
                    firstEdge.up * secondEdge.forward
                        - firstEdge.forward * secondEdge.up,
                    firstEdge.forward * secondEdge.lateral
                        - firstEdge.lateral * secondEdge.forward};
                const double normalLength = std::sqrt(
                        localNormal.forward * localNormal.forward
                        + localNormal.lateral * localNormal.lateral
                        + localNormal.up * localNormal.up);
                if (normalLength <= 1e-9) return;
                localNormal.forward /= normalLength;
                localNormal.lateral /= normalLength;
                localNormal.up /= normalLength;
                const double normalForward = localNormal.forward * cosine
                        - localNormal.lateral * sine;
                const double normalLateral = localNormal.forward * sine
                        + localNormal.lateral * cosine;
                const std::uint32_t base = std::uint32_t(vertices.size());
                for (std::size_t corner = 0; corner < face.size(); ++corner) {
                    const LocalVertex local = face[corner];
                    const double forward = local.forward * cosine
                            - local.lateral * sine;
                    const double lateral = step.lateralMeters
                            + local.forward * sine + local.lateral * cosine;
                    const float x = float(sample.center.xMeters
                            + forward * forwardX + lateral * rightX);
                    const double baseElevation =
                            sample.visualGroundElevationMeters()
                            - sample.surfaceOffsetMeters;
                    const float y = float(baseElevation
                            + forward * grade + local.up);
                    const float z = float(sample.center.zMeters
                            + forward * forwardZ + lateral * rightZ);
                    const float shade = localNormal.up > 0.25 ? 1.0f
                            : localNormal.up < -0.25 ? 0.55f : 0.78f;
                    vertices.push_back({
                        x, y, z,
                        float(normalForward * forwardX
                              + normalLateral * rightX),
                        float(localNormal.up),
                        float(normalForward * forwardZ
                              + normalLateral * rightZ),
                        0.37f * shade, 0.35f * shade, 0.30f * shade, 1.0f,
                        corner == 0 || corner == 3 ? 0.0f : 1.0f,
                        corner < 2 ? 0.0f : 1.0f
                    });
                    boundsMin.setX(std::min(boundsMin.x(), x));
                    boundsMin.setY(std::min(boundsMin.y(), y));
                    boundsMin.setZ(std::min(boundsMin.z(), z));
                    boundsMax.setX(std::max(boundsMax.x(), x));
                    boundsMax.setY(std::max(boundsMax.y(), y));
                    boundsMax.setZ(std::max(boundsMax.z(), z));
                }
                indices.insert(indices.end(), {
                    base, base + 1u, base + 2u,
                    base, base + 2u, base + 3u
                });
            };
            const std::array<std::array<LocalVertex, 4>, 6> bodyFaces = {{
                {{{-f, -w, high}, { f, -w, high},
                   { f,  w, high}, {-f,  w, high}}},
                {{{-f,  w, low}, { f,  w, low},
                   { f, -w, low}, {-f, -w, low}}},
                {{{ f, -w, low}, { f,  w, low},
                   { f,  w, high}, { f, -w, high}}},
                {{{-f,  w, low}, {-f, -w, low},
                   {-f, -w, high}, {-f,  w, high}}},
                {{{ f,  w, low}, {-f,  w, low},
                   {-f,  w, high}, { f,  w, high}}},
                {{{-f, -w, low}, { f, -w, low},
                   { f, -w, high}, {-f, -w, high}}}
            }};
            for (const auto &face : bodyFaces) appendFace(face);

            for (int side : {-1, 1}) {
                const double inner = side < 0 ? -f : f;
                const double outer = side < 0
                        ? -f - profile.contactRampMeters
                        : f + profile.contactRampMeters;
                for (int segment = 0; segment < RampSegments; ++segment) {
                    const double p0 = double(segment) / RampSegments;
                    const double p1 = double(segment + 1) / RampSegments;
                    const double forward0 = side < 0
                            ? outer + (inner - outer) * p0
                            : inner + (outer - inner) * p0;
                    const double forward1 = side < 0
                            ? outer + (inner - outer) * p1
                            : inner + (outer - inner) * p1;
                    const double top0 = profile.stepSurfaceOffsetMeters(
                            step, forward0, 0.0) + 0.006;
                    const double top1 = profile.stepSurfaceOffsetMeters(
                            step, forward1, 0.0) + 0.006;
                    appendFace({{{forward0, -w, top0},
                                 {forward1, -w, top1},
                                 {forward1,  w, top1},
                                 {forward0,  w, top0}}});
                    appendFace({{{forward0, -w, low},
                                 {forward1, -w, low},
                                 {forward1, -w, top1},
                                 {forward0, -w, top0}}});
                    appendFace({{{forward0,  w, top0},
                                 {forward1,  w, top1},
                                 {forward1,  w, low},
                                 {forward0,  w, low}}});
                }
                const double rangeStart = std::min(inner, outer);
                const double rangeEnd = std::max(inner, outer);
                appendFace({{{rangeStart,  w, low},
                             {rangeEnd,  w, low},
                             {rangeEnd, -w, low},
                             {rangeStart, -w, low}}});
                const double outerTop = profile.stepSurfaceOffsetMeters(
                        step, outer, 0.0) + 0.006;
                if (side < 0) {
                    appendFace({{{outer,  w, low}, {outer, -w, low},
                                 {outer, -w, outerTop},
                                 {outer,  w, outerTop}}});
                } else {
                    appendFace({{{outer, -w, low}, {outer,  w, low},
                                 {outer,  w, outerTop},
                                 {outer, -w, outerTop}}});
                }
            }
            Q_ASSERT(int(vertices.size())
                    == (stepCount + 1) * VerticesPerStep);
            ++stepCount;
        }
    }

    if (vertices.empty() || indices.empty()) return {};
    return meshData(
            vertices, indices, boundsMin, boundsMax, int(vertices.size()));
}

WorkoutGame3DMeshData WorkoutGame3DGeometry::buildRockGardens(
        const WorkoutGameRoadCourse &course,
        double startDistanceMeters,
        double endDistanceMeters)
{
    constexpr int Sides = 7;
    constexpr int VerticesPerStone = Sides * 2 + 1;
    constexpr int MaximumStones = 256;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    QVector3D boundsMin(
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max());
    QVector3D boundsMax(
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest());
    int stoneCount = 0;

    const auto appendVertex = [&](
            const WorkoutGameRoadSample &sample,
            double distance,
            double lateral,
            double up,
            double normalForward,
            double normalLateral,
            double normalUp,
            float shade,
            float u,
            float v) {
        const WorkoutGame3DTerrainProfileSnapshot terrain =
                WorkoutGame3DTerrainProfile::build(
                    sample, distance, course.seed);
        if (!terrain.ready) return false;
        const double rightX = std::cos(sample.center.headingRadians);
        const double rightZ = -std::sin(sample.center.headingRadians);
        const double forwardX = std::sin(sample.center.headingRadians);
        const double forwardZ = std::cos(sample.center.headingRadians);
        const double datum =
                WorkoutGame3DTerrainProfile::elevationAtLateral(
                    terrain, lateral);
        const double normalLength = std::sqrt(
                normalForward * normalForward
                + normalLateral * normalLateral
                + normalUp * normalUp);
        const double inverseNormal = normalLength > 1e-9
                ? 1.0 / normalLength : 1.0;
        const float x = float(sample.center.xMeters + lateral * rightX);
        const float y = float(datum + up);
        const float z = float(sample.center.zMeters + lateral * rightZ);
        vertices.push_back({
            x, y, z,
            float((normalForward * forwardX
                   + normalLateral * rightX) * inverseNormal),
            float(normalUp * inverseNormal),
            float((normalForward * forwardZ
                   + normalLateral * rightZ) * inverseNormal),
            0.34f + 0.17f * shade,
            0.32f + 0.14f * shade,
            0.27f + 0.10f * shade,
            1.0f, u, v
        });
        boundsMin.setX(std::min(boundsMin.x(), x));
        boundsMin.setY(std::min(boundsMin.y(), y));
        boundsMin.setZ(std::min(boundsMin.z(), z));
        boundsMax.setX(std::max(boundsMax.x(), x));
        boundsMax.setY(std::max(boundsMax.y(), y));
        boundsMax.setZ(std::max(boundsMax.z(), z));
        return true;
    };

    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (!piece.challenge.enabled
                || piece.terrain != WorkoutGameTerrainKind::RockGarden) {
            continue;
        }
        const WorkoutGameRockGardenGeometryProfile profile =
                WorkoutGameRockGardenGeometry::profile(piece.difficulty);
        if (!profile.ready) continue;
        const double center = piece.challenge.obstacleDistanceMeters;
        if (center + profile.activeEndMeters < startDistanceMeters
                || center + profile.activeStartMeters > endDistanceMeters) {
            continue;
        }
        int profileStoneIndex = 0;
        for (const WorkoutGameRockGardenStone &stone : profile.stones) {
            if (stoneCount >= MaximumStones) break;
            const std::uint32_t first = std::uint32_t(vertices.size());
            const double yawCosine = std::cos(stone.yawRadians);
            const double yawSine = std::sin(stone.yawRadians);
            const double baseUp = -profile.burialRatio * stone.heightMeters;
            for (int ring = 0; ring < 2; ++ring) {
                const double taper = ring == 0 ? 1.0 : 0.68;
                for (int side = 0; side < Sides; ++side) {
                    const double angle = 2.0 * Pi * double(side)
                            / double(Sides);
                    const double irregularity = 0.86
                            + 0.10 * std::sin(
                                3.0 * angle + 0.71 * profileStoneIndex)
                            + 0.05 * std::cos(
                                5.0 * angle - 0.37 * profileStoneIndex);
                    const double localForward = std::cos(angle)
                            * stone.forwardRadiusMeters
                            * irregularity * taper;
                    const double localLateral = std::sin(angle)
                            * stone.lateralRadiusMeters
                            * irregularity * taper;
                    const double rotatedForward =
                            localForward * yawCosine
                            - localLateral * yawSine;
                    const double rotatedLateral =
                            localForward * yawSine
                            + localLateral * yawCosine;
                    const double distance = center + stone.forwardMeters
                            + rotatedForward;
                    const double lateral = stone.lateralMeters
                            + rotatedLateral;
                    const WorkoutGameRoadSample sample =
                            WorkoutGameRoadCourseBuilder::sampleVisual(
                                course, distance);
                    if (!sample.ready) {
                        return {};
                    }
                    const double up = ring == 0
                            ? baseUp
                            : baseUp + stone.heightMeters * (
                                0.78 + 0.06 * std::sin(
                                    angle + 0.53 * profileStoneIndex));
                    if (!appendVertex(
                            sample, distance, lateral, up,
                            rotatedForward /
                                stone.forwardRadiusMeters,
                            rotatedLateral /
                                stone.lateralRadiusMeters,
                            ring == 0 ? 0.10 : 0.72,
                            float((profileStoneIndex + side) % 4) / 3.0f,
                            float(side) / float(Sides),
                            float(ring))) {
                        return {};
                    }
                }
            }
            const double crownForward = 0.08
                    * stone.forwardRadiusMeters
                    * std::sin(0.83 * profileStoneIndex);
            const double crownLateral = 0.08
                    * stone.lateralRadiusMeters
                    * std::cos(0.67 * profileStoneIndex);
            const double crownDistance = center + stone.forwardMeters
                    + crownForward;
            const double crownRight = stone.lateralMeters + crownLateral;
            const WorkoutGameRoadSample crownSample =
                    WorkoutGameRoadCourseBuilder::sampleVisual(
                        course, crownDistance);
            if (!crownSample.ready
                    || !appendVertex(
                        crownSample, crownDistance, crownRight,
                        baseUp + stone.heightMeters,
                        0.0, 0.0, 1.0,
                        float(profileStoneIndex % 4) / 3.0f,
                        0.5f, 1.0f)) {
                return {};
            }

            for (int side = 0; side < Sides; ++side) {
                const std::uint32_t next = std::uint32_t((side + 1) % Sides);
                indices.insert(indices.end(), {
                    first + std::uint32_t(side),
                    first + Sides + std::uint32_t(side),
                    first + next,
                    first + next,
                    first + Sides + std::uint32_t(side),
                    first + Sides + next,
                    first + Sides + std::uint32_t(side),
                    first + Sides * 2,
                    first + Sides + next
                });
            }
            ++stoneCount;
            ++profileStoneIndex;
        }
        if (stoneCount >= MaximumStones) break;
    }
    if (vertices.empty() || indices.empty()) return {};

    return meshData(vertices, indices, boundsMin, boundsMax,
                    stoneCount * VerticesPerStone);
}

WorkoutGame3DMeshData WorkoutGame3DGeometry::buildRockSlabs(
        const WorkoutGameRoadCourse &course,
        double startDistanceMeters,
        double endDistanceMeters)
{
    constexpr int Stations = 13;
    constexpr int Columns = 7;
    constexpr int MaximumSlabs = 12;
    constexpr int FissureCount = 4;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    QVector3D boundsMin(
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max());
    QVector3D boundsMax(
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest());
    int slabCount = 0;

    const auto appendVertex = [&](
            const WorkoutGameRoadSample &sample,
            double distance,
            double lateral,
            double up,
            double normalForward,
            double normalLateral,
            double normalUp,
            float red,
            float green,
            float blue,
            float u,
            float v) {
        const WorkoutGame3DTerrainProfileSnapshot terrain =
                WorkoutGame3DTerrainProfile::build(
                    sample, distance, course.seed);
        if (!terrain.ready) return false;
        const double rightX = std::cos(sample.center.headingRadians);
        const double rightZ = -std::sin(sample.center.headingRadians);
        const double forwardX = std::sin(sample.center.headingRadians);
        const double forwardZ = std::cos(sample.center.headingRadians);
        const double datum =
                WorkoutGame3DTerrainProfile::elevationAtLateral(
                    terrain, lateral)
                - sample.surfaceOffsetMeters;
        const double normalLength = std::sqrt(
                normalForward * normalForward
                + normalLateral * normalLateral
                + normalUp * normalUp);
        const double inverseNormal = normalLength > 1e-9
                ? 1.0 / normalLength : 1.0;
        const float x = float(sample.center.xMeters + lateral * rightX);
        const float y = float(datum + up);
        const float z = float(sample.center.zMeters + lateral * rightZ);
        vertices.push_back({
            x, y, z,
            float((normalForward * forwardX
                   + normalLateral * rightX) * inverseNormal),
            float(normalUp * inverseNormal),
            float((normalForward * forwardZ
                   + normalLateral * rightZ) * inverseNormal),
            red, green, blue, 1.0f, u, v
        });
        boundsMin.setX(std::min(boundsMin.x(), x));
        boundsMin.setY(std::min(boundsMin.y(), y));
        boundsMin.setZ(std::min(boundsMin.z(), z));
        boundsMax.setX(std::max(boundsMax.x(), x));
        boundsMax.setY(std::max(boundsMax.y(), y));
        boundsMax.setZ(std::max(boundsMax.z(), z));
        return true;
    };
    const auto position = [&vertices](std::uint32_t index) {
        const Vertex &vertex = vertices[std::size_t(index)];
        return QVector3D(vertex.x, vertex.y, vertex.z);
    };

    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (!piece.challenge.enabled
                || piece.terrain != WorkoutGameTerrainKind::RockSlab) {
            continue;
        }
        const WorkoutGameRockSlabGeometryProfile profile =
                WorkoutGameRockSlabGeometry::profile(piece.difficulty);
        if (!profile.ready) continue;
        const double center = piece.challenge.obstacleDistanceMeters;
        if (center + profile.activeEndMeters < startDistanceMeters
                || center + profile.activeStartMeters > endDistanceMeters) {
            continue;
        }
        if (slabCount >= MaximumSlabs) break;
        const std::uint32_t topStart = std::uint32_t(vertices.size());
        for (int station = 0; station < Stations; ++station) {
            const double progress = double(station) / double(Stations - 1);
            const double local = profile.activeStartMeters
                    + progress * (
                        profile.activeEndMeters
                            - profile.activeStartMeters);
            const double distance = center + local;
            const double slabCenter =
                    profile.slabCenterLateralMeters(local);
            const double halfWidth = profile.slabHalfWidthMeters(local);
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sampleVisual(course, distance);
            if (!sample.ready) {
                return {};
            }
            for (int column = 0; column < Columns; ++column) {
                const double across = -1.0
                        + 2.0 * double(column) / double(Columns - 1);
                const double lateral = slabCenter + across * halfWidth;
                const double up = profile.surfaceOffsetMeters(local, lateral);
                const float shade = float((station + column * 2) % 5) / 4.0f;
                if (!appendVertex(
                        sample, distance, lateral, up,
                        0.0, 0.0, 1.0,
                        0.28f + 0.09f * shade,
                        0.30f + 0.08f * shade,
                        0.28f + 0.07f * shade,
                        float(column) / float(Columns - 1),
                        float(progress))) {
                    return {};
                }
            }
        }
        const auto topIndex = [topStart](int station, int column) {
            return topStart + std::uint32_t(station * Columns + column);
        };
        for (int station = 0; station < Stations; ++station) {
            for (int column = 0; column < Columns; ++column) {
                const int priorStation = std::max(0, station - 1);
                const int nextStation = std::min(Stations - 1, station + 1);
                const int priorColumn = std::max(0, column - 1);
                const int nextColumn = std::min(Columns - 1, column + 1);
                const QVector3D forward =
                        position(topIndex(nextStation, column))
                        - position(topIndex(priorStation, column));
                const QVector3D right =
                        position(topIndex(station, nextColumn))
                        - position(topIndex(station, priorColumn));
                QVector3D normal = QVector3D::crossProduct(forward, right);
                if (normal.y() < 0.0f) normal = -normal;
                normal.normalize();
                Vertex &vertex = vertices[std::size_t(
                        topIndex(station, column))];
                vertex.nx = normal.x();
                vertex.ny = normal.y();
                vertex.nz = normal.z();
            }
        }
        for (int station = 0; station < Stations - 1; ++station) {
            for (int column = 0; column < Columns - 1; ++column) {
                const std::uint32_t a = topIndex(station, column);
                const std::uint32_t b = topIndex(station + 1, column);
                const std::uint32_t c = topIndex(station, column + 1);
                const std::uint32_t d = topIndex(station + 1, column + 1);
                indices.insert(indices.end(), {a, b, c, c, b, d});
            }
        }

        for (int side = 0; side < 2; ++side) {
            const int column = side == 0 ? 0 : Columns - 1;
            std::array<std::uint32_t, Stations> bottom = {};
            for (int station = 0; station < Stations; ++station) {
                const double progress =
                        double(station) / double(Stations - 1);
                const double local = profile.activeStartMeters
                        + progress * (profile.activeEndMeters
                            - profile.activeStartMeters);
                const double distance = center + local;
                const double slabCenter =
                        profile.slabCenterLateralMeters(local);
                const double lateral = slabCenter
                        + (side == 0 ? -1.0 : 1.0)
                            * profile.slabHalfWidthMeters(local);
                const WorkoutGameRoadSample sample =
                        WorkoutGameRoadCourseBuilder::sampleVisual(course, distance);
                bottom[std::size_t(station)] =
                        std::uint32_t(vertices.size());
                if (!sample.ready || !appendVertex(
                        sample, distance, lateral,
                        -profile.sideDepthMeters,
                        0.0, side == 0 ? -1.0 : 1.0, 0.12,
                        0.20f, 0.21f, 0.19f,
                        float(progress), 0.0f)) {
                    return {};
                }
            }
            for (int station = 0; station < Stations - 1; ++station) {
                const std::uint32_t a = topIndex(station, column);
                const std::uint32_t b = topIndex(station + 1, column);
                const std::uint32_t c = bottom[std::size_t(station)];
                const std::uint32_t d = bottom[std::size_t(station + 1)];
                indices.insert(indices.end(), {a, c, b, b, c, d});
            }
        }

        for (int end = 0; end < 2; ++end) {
            const int station = end == 0 ? 0 : Stations - 1;
            const double progress = double(station) / double(Stations - 1);
            const double local = profile.activeStartMeters
                    + progress * (profile.activeEndMeters
                        - profile.activeStartMeters);
            const double distance = center + local;
            const double slabCenter =
                    profile.slabCenterLateralMeters(local);
            const double halfWidth = profile.slabHalfWidthMeters(local);
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sampleVisual(course, distance);
            std::array<std::uint32_t, Columns> bottom = {};
            for (int column = 0; column < Columns; ++column) {
                const double across = -1.0
                        + 2.0 * double(column) / double(Columns - 1);
                bottom[std::size_t(column)] =
                        std::uint32_t(vertices.size());
                if (!sample.ready || !appendVertex(
                        sample, distance, slabCenter + across * halfWidth,
                        -profile.sideDepthMeters,
                        end == 0 ? -1.0 : 1.0, 0.0, 0.12,
                        0.18f, 0.19f, 0.17f,
                        float(column) / float(Columns - 1), 0.0f)) {
                    return {};
                }
            }
            for (int column = 0; column < Columns - 1; ++column) {
                const std::uint32_t a = topIndex(station, column);
                const std::uint32_t b = topIndex(station, column + 1);
                const std::uint32_t c = bottom[std::size_t(column)];
                const std::uint32_t d = bottom[std::size_t(column + 1)];
                indices.insert(indices.end(), {a, c, b, b, c, d});
            }
        }

        const std::array<double, FissureCount> fissureCenters = {
            -2.25, -0.95, 0.85, 2.20
        };
        for (int fissure = 0; fissure < FissureCount; ++fissure) {
            const double local = fissureCenters[std::size_t(fissure)];
            const double halfLength = 0.28 + 0.05 * double(fissure);
            const double halfWidth = 0.018 + 0.004 * double(fissure);
            const double direction = fissure % 2 == 0 ? 1.0 : -1.0;
            const double slabCenter =
                    profile.slabCenterLateralMeters(local);
            const double fromLateral = slabCenter - direction * 0.24;
            const double toLateral = slabCenter + direction * 0.24;
            const std::uint32_t first = std::uint32_t(vertices.size());
            for (int corner = 0; corner < 4; ++corner) {
                const bool endCorner = corner >= 2;
                const bool rightCorner = corner % 2 == 1;
                const double cornerLocal = local
                        + (endCorner ? halfLength : -halfLength);
                const double lateral = (endCorner
                        ? toLateral : fromLateral)
                        + (rightCorner ? halfWidth : -halfWidth);
                const double distance = center + cornerLocal;
                const WorkoutGameRoadSample sample =
                        WorkoutGameRoadCourseBuilder::sampleVisual(course, distance);
                if (!sample.ready || !appendVertex(
                        sample, distance, lateral,
                        profile.surfaceOffsetMeters(cornerLocal, lateral)
                            + 0.007,
                        0.0, 0.0, 1.0,
                        0.08f, 0.09f, 0.08f,
                        endCorner ? 1.0f : 0.0f,
                        rightCorner ? 1.0f : 0.0f)) {
                    return {};
                }
            }
            indices.insert(indices.end(), {
                first, first + 2u, first + 1u,
                first + 1u, first + 2u, first + 3u
            });
        }
        ++slabCount;
    }
    if (vertices.empty() || indices.empty()) return {};

    return meshData(
            vertices, indices, boundsMin, boundsMax, int(vertices.size()));
}

WorkoutGame3DMeshData WorkoutGame3DGeometry::buildSkinnies(
        const WorkoutGameRoadCourse &course,
        double startDistanceMeters,
        double endDistanceMeters)
{
    constexpr int BoardCount = 96;
    constexpr int BeamSegments = 8;
    constexpr int SupportCount = 9;
    constexpr int MaximumSkinnies = 12;
    constexpr int VerticesPerSkinny = 1584;
    constexpr int TrianglesPerSkinny = 792;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(std::size_t(MaximumSkinnies * VerticesPerSkinny));
    indices.reserve(std::size_t(
            MaximumSkinnies * TrianglesPerSkinny * 3));
    QVector3D boundsMin(
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max());
    QVector3D boundsMax(
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest());
    int skinnyCount = 0;

    struct Station
    {
        WorkoutGameRoadSample road;
        WorkoutGame3DTerrainProfileSnapshot terrain;
        bool ready = false;
    };
    std::map<double, Station> stations;

    const auto worldPoint = [&](double distance, double lateral, double up,
                                QVector3D &result) {
        auto [station, inserted] = stations.try_emplace(distance);
        if (inserted) {
            station->second.road =
                    WorkoutGameRoadCourseBuilder::sampleVisual(course, distance);
            if (station->second.road.ready) {
                station->second.terrain = WorkoutGame3DTerrainProfile::build(
                        station->second.road, distance, course.seed);
                station->second.ready = station->second.terrain.ready;
            }
        }
        if (!station->second.ready) return false;
        const WorkoutGameRoadSample &sample = station->second.road;
        const WorkoutGame3DTerrainProfileSnapshot &terrain =
                station->second.terrain;
        const double rightX = std::cos(sample.center.headingRadians);
        const double rightZ = -std::sin(sample.center.headingRadians);
        const double datum =
                WorkoutGame3DTerrainProfile::elevationAtLateral(
                    terrain, lateral)
                - sample.surfaceOffsetMeters;
        result = QVector3D(
                float(sample.center.xMeters + lateral * rightX),
                float(datum + up),
                float(sample.center.zMeters + lateral * rightZ));
        return true;
    };
    const auto appendFace = [&](const QVector3D &a, const QVector3D &b,
                                const QVector3D &c, const QVector3D &d,
                                const TerrainColor &color) {
        QVector3D normal = QVector3D::crossProduct(b - a, d - a);
        if (normal.lengthSquared() < 1e-10f) normal = QVector3D(0, 1, 0);
        normal.normalize();
        const std::uint32_t first = std::uint32_t(vertices.size());
        const std::array<QVector3D, 4> points = {a, b, c, d};
        for (int corner = 0; corner < 4; ++corner) {
            const QVector3D &point = points[std::size_t(corner)];
            vertices.push_back({
                point.x(), point.y(), point.z(),
                normal.x(), normal.y(), normal.z(),
                color.r, color.g, color.b, 1.0f,
                corner == 1 || corner == 2 ? 1.0f : 0.0f,
                corner >= 2 ? 1.0f : 0.0f
            });
            boundsMin.setX(std::min(boundsMin.x(), point.x()));
            boundsMin.setY(std::min(boundsMin.y(), point.y()));
            boundsMin.setZ(std::min(boundsMin.z(), point.z()));
            boundsMax.setX(std::max(boundsMax.x(), point.x()));
            boundsMax.setY(std::max(boundsMax.y(), point.y()));
            boundsMax.setZ(std::max(boundsMax.z(), point.z()));
        }
        indices.insert(indices.end(), {
            first, first + 1u, first + 2u,
            first, first + 2u, first + 3u
        });
    };
    const auto appendPrism = [&](double from, double to,
                                 double centerLateral, double halfWidth,
                                 double topFrom, double topTo,
                                 double bottomFrom, double bottomTo,
                                 const TerrainColor &topColor,
                                 const TerrainColor &sideColor) {
        std::array<QVector3D, 8> point;
        const bool ready =
                worldPoint(from, centerLateral - halfWidth, topFrom, point[0])
                && worldPoint(from, centerLateral + halfWidth, topFrom, point[1])
                && worldPoint(to, centerLateral + halfWidth, topTo, point[2])
                && worldPoint(to, centerLateral - halfWidth, topTo, point[3])
                && worldPoint(from, centerLateral - halfWidth,
                              bottomFrom, point[4])
                && worldPoint(from, centerLateral + halfWidth,
                              bottomFrom, point[5])
                && worldPoint(to, centerLateral + halfWidth,
                              bottomTo, point[6])
                && worldPoint(to, centerLateral - halfWidth,
                              bottomTo, point[7]);
        if (!ready) return false;
        appendFace(point[0], point[3], point[2], point[1], topColor);
        appendFace(point[4], point[5], point[6], point[7], sideColor);
        appendFace(point[0], point[4], point[7], point[3], sideColor);
        appendFace(point[1], point[2], point[6], point[5], sideColor);
        appendFace(point[0], point[1], point[5], point[4], sideColor);
        appendFace(point[3], point[7], point[6], point[2], sideColor);
        return true;
    };
    const auto appendBoard = [&](double from, double to,
                                 double halfWidth,
                                 double topFrom, double topTo,
                                 double thickness,
                                 const TerrainColor &topColor,
                                 const TerrainColor &sideColor) {
        std::array<QVector3D, 6> point;
        const bool ready =
                worldPoint(from, -halfWidth, topFrom, point[0])
                && worldPoint(from, halfWidth, topFrom, point[1])
                && worldPoint(to, halfWidth, topTo, point[2])
                && worldPoint(to, -halfWidth, topTo, point[3])
                && worldPoint(from, -halfWidth,
                              topFrom - thickness, point[4])
                && worldPoint(from, halfWidth,
                              topFrom - thickness, point[5]);
        if (!ready) return false;
        appendFace(point[0], point[3], point[2], point[1], topColor);
        appendFace(point[0], point[1], point[5], point[4], sideColor);
        return true;
    };

    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (!piece.challenge.enabled
                || piece.terrain != WorkoutGameTerrainKind::Skinny) {
            continue;
        }
        const WorkoutGameSkinnyGeometryProfile profile =
                WorkoutGameSkinnyGeometry::profile(piece.difficulty);
        const double center = piece.challenge.obstacleDistanceMeters;
        if (center + profile.activeEndMeters < startDistanceMeters
                || center + profile.activeStartMeters > endDistanceMeters) {
            continue;
        }
        if (skinnyCount >= MaximumSkinnies) break;
        const double pitch = (profile.activeEndMeters
                - profile.activeStartMeters) / double(BoardCount);
        for (int board = 0; board < BoardCount; ++board) {
            const double localFrom = profile.activeStartMeters
                    + (double(board) + 0.04) * pitch;
            const double localTo = profile.activeStartMeters
                    + (double(board) + 0.96) * pitch;
            const TerrainColor topColor = board % 4 == 0
                    ? TerrainColor{0.58f, 0.37f, 0.18f}
                    : TerrainColor{0.48f, 0.29f, 0.13f};
            const double topFrom = profile.deckSurfaceOffsetMeters(localFrom);
            const double topTo = profile.deckSurfaceOffsetMeters(localTo);
            if (!appendBoard(
                    center + localFrom, center + localTo,
                    profile.deckHalfWidthMeters,
                    topFrom, topTo, profile.deckThicknessMeters,
                    topColor, {0.28f, 0.16f, 0.07f})) {
                return {};
            }
        }
        const double beamPitch = (profile.deckEndMeters
                - profile.deckStartMeters) / double(BeamSegments);
        for (double lateral : {-0.17, 0.17}) {
            for (int segment = 0; segment < BeamSegments; ++segment) {
                const double localFrom = profile.deckStartMeters
                        + double(segment) * beamPitch;
                const double localTo = localFrom + beamPitch;
                const double top = profile.deckHeightMeters - 0.10;
                if (!appendPrism(
                        center + localFrom, center + localTo,
                        lateral, 0.045,
                        top, top, top - 0.11, top - 0.11,
                        {0.25f, 0.14f, 0.06f},
                        {0.20f, 0.10f, 0.04f})) {
                    return {};
                }
            }
        }
        for (int support = 0; support < SupportCount; ++support) {
            const double local = profile.deckStartMeters
                    + (profile.deckEndMeters - profile.deckStartMeters)
                        * (double(support) + 0.5) / double(SupportCount);
            for (double lateral : {-0.17, 0.17}) {
                const double top = profile.deckHeightMeters - 0.16;
                if (!appendPrism(
                        center + local - 0.045, center + local + 0.045,
                        lateral, 0.045,
                        top, top, 0.0, 0.0,
                        {0.24f, 0.13f, 0.05f},
                        {0.18f, 0.09f, 0.035f})) {
                    return {};
                }
            }
        }
        ++skinnyCount;
    }
    if (vertices.empty() || indices.empty()) return {};

    return meshData(
            vertices, indices, boundsMin, boundsMax, int(vertices.size()));
}

WorkoutGame3DMeshData WorkoutGame3DGeometry::buildRoots(
        const WorkoutGameRoadCourse &course,
        double startDistanceMeters,
        double endDistanceMeters)
{
    constexpr int RingsPerRoot = 5;
    constexpr int Sides = 8;
    constexpr int MaximumRootSegments = 512;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    QVector3D boundsMin(
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max());
    QVector3D boundsMax(
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest());
    int rootCount = 0;

    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (!piece.challenge.enabled
                || piece.terrain != WorkoutGameTerrainKind::Roots) {
            continue;
        }
        const WorkoutGameRootGeometryProfile profile =
                WorkoutGameRootGeometry::profile(piece.difficulty);
        if (!profile.ready) continue;
        const std::array<double, 1> centers = {{
            piece.challenge.obstacleDistanceMeters
        }};
        for (const double center : centers) {
            if (center + profile.activeEndMeters < startDistanceMeters
                    || center + profile.activeStartMeters > endDistanceMeters) {
                continue;
            }
            for (const WorkoutGameRootSegment &root : profile.segments) {
            if (rootCount >= MaximumRootSegments) break;
            const std::uint32_t first = std::uint32_t(vertices.size());
            const double localForwardDelta = root.endForwardMeters
                    - root.startForwardMeters;
            const double localLateralDelta = root.endLateralMeters
                    - root.startLateralMeters;
            for (int ring = 0; ring < RingsPerRoot; ++ring) {
                const double progress = double(ring)
                        / double(RingsPerRoot - 1);
                const double localForward = root.startForwardMeters
                        + progress * localForwardDelta;
                const double lateral = root.startLateralMeters
                        + progress * localLateralDelta;
                const double radius = root.startRadiusMeters
                        + progress * (root.endRadiusMeters
                                      - root.startRadiusMeters);
                const double distance = center + localForward;
                const WorkoutGameRoadSample sample =
                        WorkoutGameRoadCourseBuilder::sampleVisual(course, distance);
                if (!sample.ready) {
                    return {};
                }
                const double rightX = std::cos(sample.center.headingRadians);
                const double rightZ = -std::sin(sample.center.headingRadians);
                const double forwardX = std::sin(sample.center.headingRadians);
                const double forwardZ = std::cos(sample.center.headingRadians);
                double axisX = localForwardDelta * forwardX
                        + localLateralDelta * rightX;
                double axisZ = localForwardDelta * forwardZ
                        + localLateralDelta * rightZ;
                const double axisLength = std::hypot(axisX, axisZ);
                if (axisLength <= 1e-9) {
                    return {};
                }
                axisX /= axisLength;
                axisZ /= axisLength;
                const double perpendicularX = -axisZ;
                const double perpendicularZ = axisX;
                const WorkoutGame3DTerrainProfileSnapshot terrain =
                        WorkoutGame3DTerrainProfile::build(
                            sample, distance, course.seed);
                if (!terrain.ready) {
                    return {};
                }
                const double datum =
                        WorkoutGame3DTerrainProfile::elevationAtLateral(
                            terrain, lateral)
                        - sample.surfaceOffsetMeters;
                const double centerX = sample.center.xMeters
                        + lateral * rightX;
                const double centerY = datum - profile.burialRatio * radius;
                const double centerZ = sample.center.zMeters
                        + lateral * rightZ;
                for (int side = 0; side < Sides; ++side) {
                    const double angle = 2.0 * Pi * double(side)
                            / double(Sides);
                    const double horizontal = std::cos(angle);
                    const double vertical = std::sin(angle);
                    const float x = float(centerX
                            + perpendicularX * horizontal * radius);
                    const float y = float(centerY + vertical * radius);
                    const float z = float(centerZ
                            + perpendicularZ * horizontal * radius);
                    vertices.push_back({
                        x, y, z,
                        float(perpendicularX * horizontal),
                        float(vertical),
                        float(perpendicularZ * horizontal),
                        side % 3 == 0 ? 0.48f : 0.34f,
                        side % 3 == 0 ? 0.29f : 0.19f,
                        side % 3 == 0 ? 0.13f : 0.08f,
                        1.0f,
                        float(side) / float(Sides),
                        float(progress)
                    });
                    boundsMin.setX(std::min(boundsMin.x(), x));
                    boundsMin.setY(std::min(boundsMin.y(), y));
                    boundsMin.setZ(std::min(boundsMin.z(), z));
                    boundsMax.setX(std::max(boundsMax.x(), x));
                    boundsMax.setY(std::max(boundsMax.y(), y));
                    boundsMax.setZ(std::max(boundsMax.z(), z));
                }
            }
            for (int ring = 1; ring < RingsPerRoot; ++ring) {
                const std::uint32_t prior = first
                        + std::uint32_t((ring - 1) * Sides);
                const std::uint32_t current = first
                        + std::uint32_t(ring * Sides);
                for (int side = 0; side < Sides; ++side) {
                    const std::uint32_t next = std::uint32_t((side + 1) % Sides);
                    indices.insert(indices.end(), {
                        prior + std::uint32_t(side),
                        current + std::uint32_t(side),
                        prior + next,
                        prior + next,
                        current + std::uint32_t(side),
                        current + next
                    });
                }
            }
            ++rootCount;
            }
            if (rootCount >= MaximumRootSegments) break;
        }
        if (rootCount >= MaximumRootSegments) break;
    }
    if (vertices.empty() || indices.empty()) return {};

    return meshData(vertices, indices, boundsMin, boundsMax,
                    rootCount * RingsPerRoot);
}

WorkoutGame3DMeshData WorkoutGame3DGeometry::buildBerms(
        const WorkoutGameRoadCourse &course,
        double startDistanceMeters,
        double endDistanceMeters)
{
    constexpr int VerticesPerSample = 7;
    constexpr double SampleSpacingMeters = 0.15;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    QVector3D boundsMin(
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max());
    QVector3D boundsMax(
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest());
    int totalSamples = 0;

    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (piece.terrain != WorkoutGameTerrainKind::Berm) {
            continue;
        }
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(piece.difficulty);
        if (!profile.ready) continue;
        const double center = piece.geometryAnchorDistanceMeters;
        const double start = std::max(
                startDistanceMeters, center + profile.startMeters);
        const double end = std::min(
                endDistanceMeters, center + profile.endMeters);
        if (end <= start) continue;
        const int remaining = MaximumSamples - totalSamples;
        if (remaining < 2) break;
        const int count = std::clamp(
                int(std::ceil((end - start) / SampleSpacingMeters)) + 1,
                2, remaining);
        const std::uint32_t featureVertexStart =
                std::uint32_t(vertices.size());

        for (int row = 0; row < count; ++row) {
            const double distance = row + 1 == count
                    ? end
                    : start + (end - start) * double(row)
                        / double(count - 1);
            const WorkoutGameRoadSample road =
                    WorkoutGameRoadCourseBuilder::sampleVisual(course, distance);
            if (!road.ready) {
                return {};
            }
            const double local = distance - center;
            const double halfWidth = profile.halfWidthMeters(local);
            const double rightX = std::cos(road.center.headingRadians);
            const double rightZ = -std::sin(road.center.headingRadians);
            const double grade = road.center.gradePercent / 100.0;
            for (int column = 0; column < VerticesPerSample; ++column) {
                const bool leftSkirt = column == 0;
                const bool rightSkirt = column + 1 == VerticesPerSample;
                const int treadColumn = std::clamp(column - 1, 0, 4);
                const double lateral = -halfWidth
                        + 2.0 * halfWidth * double(treadColumn) / 4.0;
                const double epsilon = std::max(0.001, halfWidth * 0.01);
                const double left = profile.surfaceOffsetMeters(
                        local, lateral - epsilon, halfWidth,
                        piece.turnRadians);
                const double right = profile.surfaceOffsetMeters(
                        local, lateral + epsilon, halfWidth,
                        piece.turnRadians);
                const double crossSlope = (right - left)
                        / (2.0 * epsilon);
                const QVector3D forward{
                        float(std::sin(road.center.headingRadians)),
                        float(grade),
                        float(std::cos(road.center.headingRadians))};
                const QVector3D across{
                        float(rightX), float(crossSlope), float(rightZ)};
                QVector3D normal = QVector3D::crossProduct(forward, across);
                normal.normalize();
                const float x = float(road.center.xMeters
                        + lateral * rightX);
                const float y = float(road.visualGroundElevationMeters()
                        + profile.surfaceOffsetMeters(
                            local, lateral, halfWidth, piece.turnRadians)
                        + ((leftSkirt || rightSkirt) ? -0.020 : 0.015));
                const float z = float(road.center.zMeters
                        + lateral * rightZ);
                const double outsideDirection =
                        -std::copysign(1.0, piece.turnRadians);
                const float outside = float(std::clamp(
                        (outsideDirection * lateral / halfWidth + 1.0) * 0.5,
                        0.0, 1.0));
                const float shade = leftSkirt || rightSkirt
                        ? 0.88f
                        : 1.0f - 0.18f * outside;
                vertices.push_back({
                    x, y, z,
                    normal.x(), normal.y(), normal.z(),
                    0.50f * shade, 0.32f * shade, 0.16f * shade, 1.0f,
                    float(treadColumn) / 4.0f,
                    float(distance * 0.22)
                });
                boundsMin.setX(std::min(boundsMin.x(), x));
                boundsMin.setY(std::min(boundsMin.y(), y));
                boundsMin.setZ(std::min(boundsMin.z(), z));
                boundsMax.setX(std::max(boundsMax.x(), x));
                boundsMax.setY(std::max(boundsMax.y(), y));
                boundsMax.setZ(std::max(boundsMax.z(), z));
            }
            if (row > 0) {
                const std::uint32_t base = featureVertexStart
                        + std::uint32_t(row * VerticesPerSample);
                for (int strip = 0; strip < VerticesPerSample - 1; ++strip) {
                    const std::uint32_t offset = std::uint32_t(strip);
                    indices.insert(indices.end(), {
                        base - std::uint32_t(VerticesPerSample) + offset,
                        base + offset,
                        base - std::uint32_t(VerticesPerSample) + offset + 1u,
                        base - std::uint32_t(VerticesPerSample) + offset + 1u,
                        base + offset,
                        base + offset + 1u
                    });
                }
            }
        }
        totalSamples += count;
    }

    if (vertices.empty() || indices.empty()) return {};
    return meshData(
            vertices, indices, boundsMin, boundsMax, totalSamples);
}

WorkoutGame3DMeshData WorkoutGame3DGeometry::buildBypasses(
        const WorkoutGameRoadCourse &course,
        double startDistanceMeters,
        double endDistanceMeters)
{
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    QVector3D boundsMin(
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max());
    QVector3D boundsMax(
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest());
    int totalSamples = 0;

    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (!piece.challenge.enabled
                || !std::isfinite(piece.challenge.bypassStartDistanceMeters)
                || !std::isfinite(piece.challenge.bypassEndDistanceMeters)
                || !std::isfinite(piece.challenge.bypassLateralMeters)) {
            continue;
        }
        const double branchStart =
                piece.challenge.bypassStartDistanceMeters;
        const double branchEnd = piece.challenge.bypassEndDistanceMeters;
        const double start = std::max(startDistanceMeters, branchStart);
        const double end = std::min(endDistanceMeters, branchEnd);
        if (end <= start || branchEnd <= branchStart) continue;
        const int remaining = MaximumSamples - totalSamples;
        if (remaining < 2) break;
        const int count = std::clamp(
                int(std::ceil((end - start) /
                              MinimumSampleSpacingMeters)) + 1,
                2, remaining);
        const std::uint32_t branchVertexStart =
                std::uint32_t(vertices.size());

        for (int index = 0; index < count; ++index) {
            const double distance = index + 1 == count
                    ? end
                    : start + (end - start) * double(index)
                        / double(count - 1);
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sampleVisual(course, distance);
            const WorkoutGame3DTerrainProfileSnapshot terrain =
                    WorkoutGame3DTerrainProfile::build(
                        sample, distance, course.seed);
            if (!sample.ready || !terrain.ready) {
                return {};
            }
            const double pulse = WorkoutGameTrailBranch::blend(
                    (distance - branchStart) / (branchEnd - branchStart));
            const double center = WorkoutGameTrailBranch::lateralAt(
                    distance, branchStart, branchEnd,
                    piece.challenge.bypassLateralMeters);
            const double connectorTreadHalfWidth = std::max(
                    0.01, sample.center.halfWidthMeters
                        - BypassEdgeWidthMeters);
            const double treadHalfWidth = connectorTreadHalfWidth
                    + (BypassHalfWidthMeters - connectorTreadHalfWidth)
                        * pulse;
            const std::array<double, 4> laterals = {{
                center - treadHalfWidth - BypassEdgeWidthMeters,
                center - treadHalfWidth,
                center + treadHalfWidth,
                center + treadHalfWidth + BypassEdgeWidthMeters
            }};
            const double rightX = std::cos(sample.center.headingRadians);
            const double rightZ = -std::sin(sample.center.headingRadians);
            for (int vertexIndex = 0; vertexIndex < 4; ++vertexIndex) {
                const double lateral = laterals[std::size_t(vertexIndex)];
                const double sampleRadius = 0.04;
                const double lowElevation =
                        WorkoutGame3DTerrainProfile::elevationAtLateral(
                            terrain, lateral - sampleRadius);
                const double highElevation =
                        WorkoutGame3DTerrainProfile::elevationAtLateral(
                            terrain, lateral + sampleRadius);
                const double crossSlope = (highElevation - lowElevation)
                        / (2.0 * sampleRadius);
                const QVector3D forward(
                        float(std::sin(sample.center.headingRadians)),
                        float(sample.baseGradePercent / 100.0),
                        float(std::cos(sample.center.headingRadians)));
                const QVector3D right{
                        float(rightX), float(crossSlope), float(rightZ)};
                QVector3D normal = QVector3D::crossProduct(forward, right);
                normal.normalize();
                const bool tread = vertexIndex == 1 || vertexIndex == 2;
                const double terrainElevation =
                        WorkoutGame3DTerrainProfile::elevationAtLateral(
                            terrain, lateral);
                const double elevation = tread
                    ? WorkoutGame3DTerrainProfile::bypassSurfaceElevationMeters(
                        sample, distance, course.seed, lateral,
                        WorkoutGameTrailBranch::treadLiftMeters(pulse))
                    : terrainElevation
                        + WorkoutGameTrailBranch::edgeLiftMeters(pulse);
                const float x = float(sample.center.xMeters
                        + lateral * rightX);
                const float y = float(elevation);
                const float z = float(sample.center.zMeters
                        + lateral * rightZ);
                vertices.push_back({
                    x, y, z,
                    normal.x(), normal.y(), normal.z(),
                    tread ? 0.50f : 0.25f,
                    tread ? 0.32f : 0.32f,
                    tread ? 0.16f : 0.17f,
                    1.0f,
                    float(vertexIndex) / 3.0f,
                    float(distance * 0.22)
                });
                boundsMin.setX(std::min(boundsMin.x(), x));
                boundsMin.setY(std::min(boundsMin.y(), y));
                boundsMin.setZ(std::min(boundsMin.z(), z));
                boundsMax.setX(std::max(boundsMax.x(), x));
                boundsMax.setY(std::max(boundsMax.y(), y));
                boundsMax.setZ(std::max(boundsMax.z(), z));
            }
            if (index > 0) {
                const std::uint32_t base = branchVertexStart
                        + std::uint32_t(index * 4);
                for (std::uint32_t strip = 0; strip < 3; ++strip) {
                    indices.insert(indices.end(), {
                        base - 4u + strip,
                        base + strip,
                        base - 3u + strip,
                        base - 3u + strip,
                        base + strip,
                        base + 1u + strip
                    });
                }
            }
        }
        totalSamples += count;
    }

    if (vertices.empty() || indices.empty()) return {};
    return meshData(
            vertices, indices, boundsMin, boundsMax, totalSamples);
}
