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
#include "WorkoutGame3DTerrainProfile.h"
#include "WorkoutGameFeatureGeometry.h"
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
#include <vector>

namespace {

constexpr int MaximumSamples = 16000;
constexpr double MinimumSampleSpacingMeters = 0.75;
constexpr double BypassHalfWidthMeters = 0.38;
constexpr double BypassEdgeWidthMeters = 0.08;
constexpr double Pi = 3.14159265358979323846;

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
    case WorkoutGameTerrainKind::RockSlab: return {0.50f, 0.48f, 0.43f};
    case WorkoutGameTerrainKind::Climb: return {0.49f, 0.31f, 0.15f};
    default: return {0.50f, 0.32f, 0.16f};
    }
}

void appendBytes(QByteArray &data, const void *source, std::size_t size)
{
    data.append(static_cast<const char *>(source), qsizetype(size));
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
        if (!piece.challenge.enabled
                || piece.terrain != WorkoutGameTerrainKind::Berm) {
            continue;
        }
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(piece.difficulty);
        const double center = piece.challenge.obstacleDistanceMeters;
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
    build(course, 0.0, course.totalLengthMeters);
}

void WorkoutGame3DGeometry::setCourseRange(
        const WorkoutGameRoadCourse &course,
        double startDistanceMeters,
        double endDistanceMeters)
{
    build(course, startDistanceMeters, endDistanceMeters);
}

void WorkoutGame3DGeometry::build(
        const WorkoutGameRoadCourse &course,
        double requestedStartDistanceMeters,
        double requestedEndDistanceMeters)
{
    clear();
    update();
    geometryReady = false;
    generatedSampleCount = 0;
    if (!course.ready || course.totalLengthMeters <= 0.0) return;

    const double startDistanceMeters = std::clamp(
            requestedStartDistanceMeters, 0.0, course.totalLengthMeters);
    const double endDistanceMeters = std::clamp(
            requestedEndDistanceMeters, 0.0, course.totalLengthMeters);
    if (!std::isfinite(startDistanceMeters)
            || !std::isfinite(endDistanceMeters)
            || endDistanceMeters <= startDistanceMeters) {
        return;
    }
    if (layer == Layer::Bypass) {
        buildBypasses(course, startDistanceMeters, endDistanceMeters);
        return;
    }
    if (layer == Layer::Berm) {
        buildBerms(course, startDistanceMeters, endDistanceMeters);
        return;
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
    vertices.reserve(std::size_t(count * verticesPerSample));
    rideableSamples.reserve(std::size_t(count));
    renderableTrailSamples.reserve(std::size_t(count));
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
                WorkoutGameRoadCourseBuilder::sample(course, distance);
        if (!sample.ready) {
            clear();
            return;
        }
        rideableSamples.push_back(sample.rideableSurface);
        renderableTrailSamples.push_back(sample.renderableTrailSurface);
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
            clear();
            return;
        }
        for (int vertex = 0; vertex < verticesPerSample; ++vertex) {
            const bool trailVertex = layer == Layer::Trail;
            const double lateral = trailVertex
                    ? (vertex == 0
                        ? -sample.center.halfWidthMeters
                        : sample.center.halfWidthMeters)
                    : terrain.vertices[std::size_t(vertex)].lateralMeters;
            const double elevation = trailVertex
                    ? sample.visualGroundElevationMeters() + 0.015
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
                    && renderableTrailSamples[std::size_t(index - 1)]
                    && renderableTrailSamples[std::size_t(index)]))) {
            const std::uint32_t base = std::uint32_t(
                    index * verticesPerSample);
            for (int strip = 0; strip < stripsPerSample; ++strip) {
                if (layer == Layer::ForestFloor && strip == 3
                        && (!renderableTrailSamples[std::size_t(index - 1)]
                            || !renderableTrailSamples[std::size_t(index)])) {
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

    QByteArray vertexData;
    vertexData.reserve(qsizetype(vertices.size() * sizeof(Vertex)));
    appendBytes(vertexData, vertices.data(), vertices.size() * sizeof(Vertex));
    QByteArray indexData;
    indexData.reserve(qsizetype(indices.size() * sizeof(std::uint32_t)));
    appendBytes(indexData, indices.data(), indices.size() * sizeof(std::uint32_t));

    setStride(sizeof(Vertex));
    setVertexData(vertexData);
    setIndexData(indexData);
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
    setBounds(boundsMin, boundsMax);
    geometryReady = true;
    generatedSampleCount = count;
    update();
}

void WorkoutGame3DGeometry::buildBerms(
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
        if (!piece.challenge.enabled
                || piece.terrain != WorkoutGameTerrainKind::Berm) {
            continue;
        }
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(piece.difficulty);
        if (!profile.ready) continue;
        const double center = piece.challenge.obstacleDistanceMeters;
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
                    WorkoutGameRoadCourseBuilder::sample(course, distance);
            if (!road.ready) {
                clear();
                return;
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

    if (vertices.empty() || indices.empty()) return;
    QByteArray vertexData;
    vertexData.reserve(qsizetype(vertices.size() * sizeof(Vertex)));
    appendBytes(vertexData, vertices.data(), vertices.size() * sizeof(Vertex));
    QByteArray indexData;
    indexData.reserve(qsizetype(indices.size() * sizeof(std::uint32_t)));
    appendBytes(indexData, indices.data(), indices.size() * sizeof(std::uint32_t));
    setStride(sizeof(Vertex));
    setVertexData(vertexData);
    setIndexData(indexData);
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
    setBounds(boundsMin, boundsMax);
    geometryReady = true;
    generatedSampleCount = totalSamples;
    update();
}

void WorkoutGame3DGeometry::buildBypasses(
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
                    WorkoutGameRoadCourseBuilder::sample(course, distance);
            const WorkoutGame3DTerrainProfileSnapshot terrain =
                    WorkoutGame3DTerrainProfile::build(
                        sample, distance, course.seed);
            if (!sample.ready || !terrain.ready) {
                clear();
                return;
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
                const double elevation =
                        WorkoutGame3DTerrainProfile::elevationAtLateral(
                            terrain, lateral)
                        + (tread
                            ? WorkoutGameTrailBranch::treadLiftMeters(pulse)
                            : WorkoutGameTrailBranch::edgeLiftMeters(pulse));
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

    if (vertices.empty() || indices.empty()) return;
    QByteArray vertexData;
    vertexData.reserve(qsizetype(vertices.size() * sizeof(Vertex)));
    appendBytes(vertexData, vertices.data(), vertices.size() * sizeof(Vertex));
    QByteArray indexData;
    indexData.reserve(qsizetype(indices.size() * sizeof(std::uint32_t)));
    appendBytes(indexData, indices.data(), indices.size() * sizeof(std::uint32_t));
    setStride(sizeof(Vertex));
    setVertexData(vertexData);
    setIndexData(indexData);
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
    setBounds(boundsMin, boundsMax);
    geometryReady = true;
    generatedSampleCount = totalSamples;
    update();
}
