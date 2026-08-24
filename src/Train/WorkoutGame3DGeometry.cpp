/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DGeometry.h"

#include "WorkoutGameFeatureGeometry.h"

#include <QByteArray>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr int MaximumSamples = 16000;
constexpr double MinimumSampleSpacingMeters = 0.75;
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
    const int verticesPerSample = layer == Layer::Trail ? 2 : 4;
    const int stripsPerSample = layer == Layer::Trail ? 1 : 2;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(std::size_t(count * verticesPerSample));
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
        const double rightX = std::cos(sample.center.headingRadians);
        const double rightZ = -std::sin(sample.center.headingRadians);
        const TerrainColor color = layer == Layer::Trail
                ? colorFor(sample.terrain)
                : TerrainColor{0.20f, 0.32f, 0.17f};
        const double innerWidth = std::max(
                0.0, sample.center.halfWidthMeters - 0.04);
        const double lateralOffsets[] = {
            -14.0, -innerWidth, innerWidth, 14.0
        };
        for (int vertex = 0; vertex < verticesPerSample; ++vertex) {
            const bool trailVertex = layer == Layer::Trail;
            const bool innerFloorVertex = !trailVertex
                    && (vertex == 1 || vertex == 2);
            const double lateral = trailVertex
                    ? (vertex == 0
                        ? -sample.center.halfWidthMeters
                        : sample.center.halfWidthMeters)
                    : lateralOffsets[vertex];
            const double elevation = trailVertex || innerFloorVertex
                    ? sample.center.elevationMeters
                    : sample.baseElevationMeters;
            const double verticalOffset = trailVertex ? 0.015 : -0.09;
            const double grade = (trailVertex || innerFloorVertex
                    ? sample.center.gradePercent
                    : sample.baseGradePercent) / 100.0;
            QVector3D normal(
                    float(-std::sin(sample.center.headingRadians) * grade),
                    1.0f,
                    float(-std::cos(sample.center.headingRadians) * grade));
            normal.normalize();
            const float x = float(sample.center.xMeters
                    + lateral * rightX);
            const float y = float(elevation + verticalOffset);
            const float z = float(sample.center.zMeters
                    + lateral * rightZ);
            vertices.push_back({
                x, y, z,
                normal.x(), normal.y(), normal.z(),
                color.r, color.g, color.b, 1.0f,
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
        if (index > 0) {
            const std::uint32_t base = std::uint32_t(
                    index * verticesPerSample);
            const int stripStarts[] = {0, 2};
            for (int strip = 0; strip < stripsPerSample; ++strip) {
                const std::uint32_t left = std::uint32_t(
                        stripStarts[strip]);
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
}
