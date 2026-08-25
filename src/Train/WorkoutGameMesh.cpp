/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameMesh.h"

#include "WorkoutGameFeatureGeometry.h"
#include "WorkoutGameTrailBranch.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double Pi = 3.14159265358979323846;

double smoothStep(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    return clamped * clamped * (3.0 - 2.0 * clamped);
}

std::uint32_t addVertex(
        WorkoutGameMesh &mesh,
        double forward,
        double right,
        double up,
        double u,
        double v)
{
    mesh.vertices.push_back({forward, right, up, u, v});
    return std::uint32_t(mesh.vertices.size() - 1u);
}

void addTriangle(
        WorkoutGameMesh &mesh,
        std::uint32_t a,
        std::uint32_t b,
        std::uint32_t c,
        WorkoutGameMeshMaterial material)
{
    mesh.triangles.push_back({{a, b, c}, material});
}

void addQuad(
        WorkoutGameMesh &mesh,
        std::uint32_t a,
        std::uint32_t b,
        std::uint32_t c,
        std::uint32_t d,
        WorkoutGameMeshMaterial material)
{
    addTriangle(mesh, a, b, c, material);
    addTriangle(mesh, a, c, d, material);
}

void addBox(
        WorkoutGameMesh &mesh,
        double forward,
        double right,
        double up,
        double halfForward,
        double halfRight,
        double halfUp,
        WorkoutGameMeshMaterial side,
        WorkoutGameMeshMaterial top)
{
    const std::uint32_t start = std::uint32_t(mesh.vertices.size());
    for (int vertical = -1; vertical <= 1; vertical += 2) {
        for (int lateral = -1; lateral <= 1; lateral += 2) {
            for (int longitudinal = -1; longitudinal <= 1;
                 longitudinal += 2) {
                addVertex(mesh,
                        forward + longitudinal * halfForward,
                        right + lateral * halfRight,
                        up + vertical * halfUp,
                        longitudinal > 0 ? 1.0 : 0.0,
                        lateral > 0 ? 1.0 : 0.0);
            }
        }
    }
    const auto vertex = [start](int vertical, int lateral, int longitudinal) {
        return start + std::uint32_t(
                vertical * 4 + lateral * 2 + longitudinal);
    };
    addQuad(mesh, vertex(1, 0, 0), vertex(1, 0, 1),
            vertex(1, 1, 1), vertex(1, 1, 0), top);
    addQuad(mesh, vertex(0, 1, 0), vertex(0, 1, 1),
            vertex(0, 0, 1), vertex(0, 0, 0), side);
    addQuad(mesh, vertex(0, 0, 0), vertex(0, 0, 1),
            vertex(1, 0, 1), vertex(1, 0, 0), side);
    addQuad(mesh, vertex(0, 1, 1), vertex(0, 1, 0),
            vertex(1, 1, 0), vertex(1, 1, 1), side);
    addQuad(mesh, vertex(0, 0, 1), vertex(0, 1, 1),
            vertex(1, 1, 1), vertex(1, 0, 1), side);
    addQuad(mesh, vertex(0, 1, 0), vertex(0, 0, 0),
            vertex(1, 0, 0), vertex(1, 1, 0), side);
}

struct SculptedStripSample
{
    double forward = 0.0;
    double up = 0.0;
    double halfWidth = 1.0;
    double centerRight = 0.0;
};

void addSculptedStrip(
        WorkoutGameMesh &mesh,
        const std::vector<SculptedStripSample> &samples,
        double thickness,
        WorkoutGameMeshMaterial top,
        WorkoutGameMeshMaterial highlight,
        WorkoutGameMeshMaterial side,
        bool fillToGround = false,
        int highlightStride = 3)
{
    if (samples.size() < 2u) return;
    const std::uint32_t start = std::uint32_t(mesh.vertices.size());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const SculptedStripSample &sample = samples[index];
        const double progress = double(index) / double(samples.size() - 1u);
        addVertex(mesh, sample.forward,
                  sample.centerRight - sample.halfWidth,
                  sample.up, progress, 0.0);
        addVertex(mesh, sample.forward,
                  sample.centerRight + sample.halfWidth,
                  sample.up, progress, 1.0);
        const double bottomUp = fillToGround
                ? std::min(0.0, sample.up) : sample.up - thickness;
        addVertex(mesh, sample.forward,
                  sample.centerRight - sample.halfWidth,
                  bottomUp, progress, 0.0);
        addVertex(mesh, sample.forward,
                  sample.centerRight + sample.halfWidth,
                  bottomUp, progress, 1.0);
        if (index == 0u) continue;
        const std::uint32_t current = start + std::uint32_t(index * 4u);
        const WorkoutGameMeshMaterial topMaterial = highlightStride > 0
                && index % std::size_t(highlightStride) == 0u
                ? highlight : top;
        addQuad(mesh, current - 4, current - 3,
                current + 1, current, topMaterial);
        addQuad(mesh, current - 2, current - 4,
                current, current + 2, side);
        addQuad(mesh, current - 3, current - 1,
                current + 3, current + 1, side);
    }
    addQuad(mesh, start + 2, start + 3, start + 1, start, side);
    const std::uint32_t end = start
            + std::uint32_t((samples.size() - 1u) * 4u);
    addQuad(mesh, end, end + 1, end + 3, end + 2, side);
}

void addIrregularBoulder(
        WorkoutGameMesh &mesh,
        double forward,
        double right,
        double length,
        double width,
        double height,
        double phase)
{
    constexpr int Sides = 7;
    const std::uint32_t start = std::uint32_t(mesh.vertices.size());
    for (int ring = 0; ring < 2; ++ring) {
        for (int index = 0; index < Sides; ++index) {
            const double angle = 2.0 * Pi * double(index) / double(Sides);
            const double irregularity = 0.84
                    + 0.14 * std::sin(3.0 * angle + phase)
                    + 0.07 * std::cos(5.0 * angle - phase);
            const double taper = ring == 0 ? 1.0 : 0.67;
            addVertex(mesh,
                    forward + std::cos(angle) * length * 0.5
                        * irregularity * taper,
                    right + std::sin(angle) * width * 0.5
                        * irregularity * taper,
                    ring == 0 ? 0.0
                              : height * (0.80
                                  + 0.08 * std::sin(angle + phase)),
                    0.5 + 0.5 * std::cos(angle),
                    0.5 + 0.5 * std::sin(angle));
        }
    }
    const std::uint32_t crown = addVertex(
            mesh,
            forward + 0.08 * length * std::sin(phase),
            right + 0.07 * width * std::cos(phase),
            height * 0.94, 0.5, 0.5);
    for (int index = 0; index < Sides; ++index) {
        const int next = (index + 1) % Sides;
        addQuad(mesh,
                start + std::uint32_t(index),
                start + std::uint32_t(next),
                start + std::uint32_t(Sides + next),
                start + std::uint32_t(Sides + index),
                index % 3 == 0
                    ? WorkoutGameMeshMaterial::RockHighlight
                    : WorkoutGameMeshMaterial::RockSide);
        addTriangle(mesh,
                start + std::uint32_t(Sides + index),
                start + std::uint32_t(Sides + next),
                crown,
                index % 2 == 0
                    ? WorkoutGameMeshMaterial::RockTop
                    : WorkoutGameMeshMaterial::RockHighlight);
    }
}

void addRootSegment(
        WorkoutGameMesh &mesh,
        double fromForward,
        double fromRight,
        double toForward,
        double toRight,
        double fromRadius,
        double toRadius)
{
    constexpr int Sides = 8;
    const double deltaForward = toForward - fromForward;
    const double deltaRight = toRight - fromRight;
    const double length = std::hypot(deltaForward, deltaRight);
    if (length <= 1e-6) return;
    const double perpendicularForward = -deltaRight / length;
    const double perpendicularRight = deltaForward / length;
    const std::uint32_t start = std::uint32_t(mesh.vertices.size());
    for (int end = 0; end < 2; ++end) {
        const double radius = end == 0 ? fromRadius : toRadius;
        const double centerForward = end == 0 ? fromForward : toForward;
        const double centerRight = end == 0 ? fromRight : toRight;
        for (int index = 0; index < Sides; ++index) {
            const double angle = 2.0 * Pi * double(index) / double(Sides);
            const double sideOffset = std::cos(angle) * radius;
            addVertex(mesh,
                    centerForward + perpendicularForward * sideOffset,
                    centerRight + perpendicularRight * sideOffset,
                    radius * 0.72 + std::sin(angle) * radius,
                    double(end), double(index) / double(Sides));
        }
    }
    for (int index = 0; index < Sides; ++index) {
        const int next = (index + 1) % Sides;
        addQuad(mesh,
                start + std::uint32_t(index),
                start + std::uint32_t(next),
                start + std::uint32_t(Sides + next),
                start + std::uint32_t(Sides + index),
                index % 3 == 0
                    ? WorkoutGameMeshMaterial::WoodHighlight
                    : WorkoutGameMeshMaterial::WoodSide);
    }
}

WorkoutGameMesh logModel(double difficulty)
{
    WorkoutGameMesh mesh;
    const WorkoutGameFeatureGeometryProfile profile =
            WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::LogOver, difficulty);
    const double radius = profile.heightMeters * 0.5;
    constexpr int Rings = 7;
    const double right[Rings] = {
        -0.95, -0.73, -0.40, 0.0, 0.39, 0.72, 0.95
    };
    const double radiusScale[Rings] = {
        0.56, 0.80, 0.94, 1.0, 0.91, 0.74, 0.50
    };
    const double forwardOffset[Rings] = {
        0.018, -0.012, 0.010, 0.0, -0.014, 0.012, -0.008
    };
    for (int ring = 0; ring < Rings; ++ring) {
        const double localRadius = radius * radiusScale[ring];
        for (int index = 0; index < WorkoutGameLogRadialSegments; ++index) {
            const double angle = 2.0 * Pi * double(index)
                    / double(WorkoutGameLogRadialSegments);
            const double verticalScale = std::sin(angle) >= 0.0
                    ? profile.heightMeters : profile.heightMeters * 0.13;
            addVertex(mesh,
                    forwardOffset[ring] + std::cos(angle) * localRadius,
                    right[ring],
                    std::sin(angle) * verticalScale * radiusScale[ring],
                    double(ring) / double(Rings - 1),
                    double(index) / double(WorkoutGameLogRadialSegments));
        }
        if (ring == 0) continue;
        const std::uint32_t current = std::uint32_t(
                ring * WorkoutGameLogRadialSegments);
        for (int index = 0; index < WorkoutGameLogRadialSegments; ++index) {
            const int next = (index + 1) % WorkoutGameLogRadialSegments;
            addQuad(mesh,
                    current - WorkoutGameLogRadialSegments
                        + std::uint32_t(index),
                    current - WorkoutGameLogRadialSegments
                        + std::uint32_t(next),
                    current + std::uint32_t(next),
                    current + std::uint32_t(index),
                    (ring + index) % 4 == 0
                        ? WorkoutGameMeshMaterial::WoodHighlight
                        : WorkoutGameMeshMaterial::WoodSide);
        }
    }
    const std::uint32_t leftCenter = addVertex(
            mesh, forwardOffset[0], right[0], 0.0,
            0.0, 0.5);
    const std::uint32_t rightCenter = addVertex(
            mesh, forwardOffset[Rings - 1], right[Rings - 1], 0.0,
            1.0, 0.5);
    const std::uint32_t lastRing = std::uint32_t(
            (Rings - 1) * WorkoutGameLogRadialSegments);
    for (int index = 0; index < WorkoutGameLogRadialSegments; ++index) {
        const int next = (index + 1) % WorkoutGameLogRadialSegments;
        addTriangle(mesh, leftCenter,
                std::uint32_t(next), std::uint32_t(index),
                WorkoutGameMeshMaterial::WoodTop);
        addTriangle(mesh, rightCenter,
                lastRing + std::uint32_t(index),
                lastRing + std::uint32_t(next),
                WorkoutGameMeshMaterial::WoodTop);
    }
    mesh.colliders.push_back({
        0.0, 0.0, radius,
        radius, 0.95, radius
    });
    mesh.lengthMeters = radius * 2.0;
    mesh.entry = {-radius, 0.95, 0.0};
    mesh.exit = {radius, 0.95, 0.0};
    mesh.ready = true;
    return mesh;
}

WorkoutGameMesh bunnyHopModel(double difficulty)
{
    WorkoutGameMesh mesh;
    const WorkoutGameFeatureGeometryProfile profile =
            WorkoutGameFeatureGeometry::profile(
                WorkoutGameTerrainKind::BunnyHop, difficulty);
    if (!profile.ready) return mesh;
    const double beamHalfForward =
            (profile.plateauEndMeters - profile.plateauStartMeters) * 0.5;
    const double beamHalfHeight = 0.045;
    addBox(mesh, 0.0, 0.0, profile.heightMeters - beamHalfHeight,
           beamHalfForward, 1.02, beamHalfHeight,
           WorkoutGameMeshMaterial::WoodSide,
           WorkoutGameMeshMaterial::WoodHighlight);
    for (double side : {-1.0, 1.0}) {
        addBox(mesh, 0.0, side * 0.84, profile.heightMeters * 0.47,
               0.045, 0.055, profile.heightMeters * 0.47,
               WorkoutGameMeshMaterial::WoodSide,
               WorkoutGameMeshMaterial::WoodHighlight);
        addBox(mesh, 0.0, side * 0.84, 0.035,
               0.28, 0.11, 0.035,
               WorkoutGameMeshMaterial::WoodSide,
               WorkoutGameMeshMaterial::WoodTop);
    }
    mesh.colliders.push_back({
        0.0, 0.0, profile.heightMeters - beamHalfHeight,
        beamHalfForward, 1.02, beamHalfHeight
    });
    mesh.lengthMeters = profile.endMeters - profile.startMeters;
    mesh.entry = {profile.startMeters, 1.02, 0.0};
    mesh.exit = {profile.endMeters, 1.02, 0.0};
    mesh.ready = true;
    return mesh;
}

WorkoutGameMesh tabletopModel(double difficulty)
{
    WorkoutGameMesh mesh;
    const WorkoutGameFeatureGeometryProfile profile =
            WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::Tabletop, difficulty);
    const double height = profile.heightMeters;
    constexpr int Samples = 24;
    std::vector<SculptedStripSample> samples;
    samples.reserve(Samples + 1u);
    for (int section = 0; section <= Samples; ++section) {
        const double progress = double(section) / double(Samples);
        const double forward = profile.startMeters
                + progress * (profile.endMeters - profile.startMeters);
        const double up = profile.surfaceOffset(forward);
        const double shoulder = std::pow(std::sin(Pi * progress), 2.0);
        samples.push_back({
            forward,
            up,
            0.92 + 0.16 * shoulder
                + 0.025 * std::sin(5.0 * Pi * progress),
            0.025 * std::sin(3.0 * Pi * progress)
        });
    }
    addSculptedStrip(
            mesh, samples, 0.24,
            WorkoutGameMeshMaterial::Dirt,
            WorkoutGameMeshMaterial::DirtHighlight,
            WorkoutGameMeshMaterial::DirtEdge,
            true,
            8);
    mesh.colliders.push_back({
        0.0, 0.0, height * 0.5,
        (profile.endMeters - profile.startMeters) * 0.5,
        1.10, height * 0.5
    });
    mesh.lengthMeters = profile.endMeters - profile.startMeters;
    mesh.entry = {profile.startMeters, 1.10, 0.0};
    mesh.exit = {profile.endMeters, 1.10, 0.0};
    mesh.ready = true;
    return mesh;
}

WorkoutGameMesh rockSlabModel(double difficulty)
{
    WorkoutGameMesh mesh;
    const WorkoutGameFeatureGeometryProfile profile =
            WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::RockSlab, difficulty);
    const double height = profile.heightMeters;
    std::vector<SculptedStripSample> samples;
    constexpr int Samples = 10;
    for (int index = 0; index <= Samples; ++index) {
        const double progress = double(index) / double(Samples);
        const double forward = profile.startMeters
                + progress * (profile.endMeters - profile.startMeters);
        samples.push_back({
            forward,
            profile.surfaceOffset(forward),
            1.05 + 0.18 * std::sin(Pi * progress)
                + 0.06 * std::sin(5.0 * Pi * progress),
            0.05 * std::sin(4.0 * Pi * progress)
        });
    }
    addSculptedStrip(
            mesh, samples, 0.22,
            WorkoutGameMeshMaterial::RockTop,
            WorkoutGameMeshMaterial::RockHighlight,
            WorkoutGameMeshMaterial::RockSide,
            true);
    mesh.colliders.push_back({
        1.5, 0.0, height * 0.5,
        4.5, 1.30, height * 0.5
    });
    mesh.lengthMeters = profile.endMeters - profile.startMeters;
    mesh.entry = {profile.startMeters, 1.30, 0.0};
    mesh.exit = {profile.endMeters, 1.30, 0.0};
    mesh.ready = true;
    return mesh;
}

WorkoutGameMesh roughModel(
        WorkoutGameTerrainKind terrain,
        double difficulty)
{
    WorkoutGameMesh mesh;
    if (terrain == WorkoutGameTerrainKind::Roots) {
        const double scale = 1.0 + 0.22 * difficulty;
        addRootSegment(mesh, -1.7, -1.25, -0.35, -0.18,
                       0.105 * scale, 0.075 * scale);
        addRootSegment(mesh, -0.35, -0.18, 1.55, 1.16,
                       0.075 * scale, 0.045 * scale);
        addRootSegment(mesh, -0.52, -0.30, 0.55, -1.24,
                       0.080 * scale, 0.035 * scale);
        addRootSegment(mesh, -1.05, 1.25, -0.10, 0.18,
                       0.090 * scale, 0.060 * scale);
        addRootSegment(mesh, -0.10, 0.18, 1.32, -1.12,
                       0.060 * scale, 0.035 * scale);
        addRootSegment(mesh, 0.42, 1.20, 1.42, 0.30,
                       0.080 * scale, 0.040 * scale);
        addRootSegment(mesh, -1.42, 0.42, -0.72, 1.28,
                       0.070 * scale, 0.030 * scale);
        for (int index = -2; index <= 2; ++index) {
            mesh.colliders.push_back({
                double(index) * 0.65, 0.0, 0.07 * scale,
                0.18, 1.20, 0.07 * scale
            });
        }
        mesh.lengthMeters = 3.6;
    } else {
        for (int index = 0; index < 9; ++index) {
            const double forward = -2.65 + double(index) * 0.67;
            const double right = ((index * 5) % 9 - 4) * 0.27;
            const double size = 0.27 + 0.07 * double(index % 4)
                    + 0.12 * difficulty;
            const double rockHeight = size * (0.62
                    + 0.06 * double(index % 3));
            addIrregularBoulder(
                    mesh, forward, right,
                    size * (1.45 + 0.12 * double(index % 2)),
                    size * (1.55 + 0.10 * double((index + 1) % 3)),
                    rockHeight, 0.73 * double(index));
            mesh.colliders.push_back({
                forward, right, rockHeight * 0.5,
                size * 0.75, size * 0.82, rockHeight * 0.5
            });
        }
        mesh.lengthMeters = 5.8;
    }
    mesh.entry = {-mesh.lengthMeters * 0.5, 1.4, 0.0};
    mesh.exit = {mesh.lengthMeters * 0.5, 1.4, 0.0};
    mesh.ready = true;
    return mesh;
}

WorkoutGameMesh rollersModel(double difficulty)
{
    WorkoutGameMesh mesh;
    std::vector<SculptedStripSample> samples;
    constexpr int Samples = 24;
    constexpr double Length = 7.2;
    for (int index = 0; index <= Samples; ++index) {
        const double progress = double(index) / double(Samples);
        const double envelope = std::pow(std::sin(Pi * progress), 2.0);
        const double humps = std::pow(
                std::max(0.0, std::sin(3.0 * Pi * progress)), 1.35);
        samples.push_back({
            -Length * 0.5 + Length * progress,
            (0.18 + 0.12 * difficulty) * envelope * humps,
            0.92 + 0.06 * std::sin(2.0 * Pi * progress),
            0.03 * std::sin(4.0 * Pi * progress)
        });
    }
    addSculptedStrip(
            mesh, samples, 0.16,
            WorkoutGameMeshMaterial::Dirt,
            WorkoutGameMeshMaterial::DirtHighlight,
            WorkoutGameMeshMaterial::DirtEdge,
            true);
    mesh.colliders.push_back({0.0, 0.0, 0.12, 3.6, 1.0, 0.18});
    mesh.lengthMeters = Length;
    mesh.entry = {-Length * 0.5, 1.0, 0.0};
    mesh.exit = {Length * 0.5, 1.0, 0.0};
    mesh.ready = true;
    return mesh;
}

WorkoutGameMesh climbModel(double difficulty)
{
    WorkoutGameMesh mesh;
    for (int index = 0; index < 7; ++index) {
        const double forward = -3.0 + double(index);
        const double side = index % 2 == 0 ? -1.0 : 1.0;
        const double right = side * (0.82 + 0.13 * double(index % 3));
        const double size = 0.20 + 0.035 * double(index % 4)
                + 0.08 * difficulty;
        addIrregularBoulder(
                mesh, forward, right,
                size * 1.35, size * 1.15, size,
                0.91 * double(index));
        mesh.colliders.push_back({
            forward, right, size * 0.5,
            size * 0.7, size * 0.6, size * 0.5
        });
    }
    mesh.lengthMeters = 6.5;
    mesh.entry = {-3.25, 1.25, 0.0};
    mesh.exit = {3.25, 1.25, 0.0};
    mesh.ready = true;
    return mesh;
}

WorkoutGameMesh skinnyModel(double difficulty)
{
    WorkoutGameMesh mesh;
    std::vector<SculptedStripSample> samples;
    constexpr int Samples = 12;
    constexpr double Length = 7.0;
    for (int index = 0; index <= Samples; ++index) {
        const double progress = double(index) / double(Samples);
        samples.push_back({
            -Length * 0.5 + Length * progress,
            0.22 + 0.025 * std::sin(4.0 * Pi * progress),
            0.30 + 0.035 * difficulty
                + 0.018 * std::sin(5.0 * Pi * progress),
            0.03 * std::sin(2.0 * Pi * progress)
        });
    }
    addSculptedStrip(
            mesh, samples, 0.18,
            WorkoutGameMeshMaterial::WoodTop,
            WorkoutGameMeshMaterial::WoodHighlight,
            WorkoutGameMeshMaterial::WoodSide);
    mesh.colliders.push_back({0.0, 0.0, 0.12, 3.5, 0.36, 0.12});
    mesh.lengthMeters = Length;
    mesh.entry = {-Length * 0.5, 0.38, 0.0};
    mesh.exit = {Length * 0.5, 0.38, 0.0};
    mesh.ready = true;
    return mesh;
}

WorkoutGameMesh bermModel(double difficulty)
{
    WorkoutGameMesh mesh;
    constexpr int Samples = 18;
    const double halfLength = 3.6 * std::sin(0.70);
    const std::uint32_t start = std::uint32_t(mesh.vertices.size());
    for (int index = 0; index <= Samples; ++index) {
        const double progress = double(index) / double(Samples);
        const double angle = -0.70 + 1.40 * progress;
        const double forward = 3.6 * std::sin(angle);
        const double centerRight = 1.15 - 3.6 * (1.0 - std::cos(angle));
        const double bank = (0.42 + 0.38 * difficulty)
                * std::pow(std::sin(Pi * progress), 0.75);
        addVertex(mesh, forward, centerRight - 0.62,
                  0.02, progress, 0.0);
        addVertex(mesh, forward, centerRight + 0.62,
                  bank, progress, 1.0);
        addVertex(mesh, forward, centerRight + 0.78,
                  bank - 0.22, progress, 1.0);
        if (index == 0) continue;
        const std::uint32_t current = start + std::uint32_t(index * 3);
        addQuad(mesh, current - 3, current - 2,
                current + 1, current,
                index % 3 == 0
                    ? WorkoutGameMeshMaterial::DirtHighlight
                    : WorkoutGameMeshMaterial::Dirt);
        addQuad(mesh, current - 2, current - 1,
                current + 2, current + 1,
                WorkoutGameMeshMaterial::DirtEdge);
    }
    mesh.colliders.push_back({0.0, 0.8, 0.3, 2.5, 1.2, 0.45});
    mesh.lengthMeters = 2.0 * halfLength;
    mesh.entry = {-halfLength, 1.4, 0.0};
    mesh.exit = {halfLength, 1.4, 0.0};
    mesh.ready = true;
    return mesh;
}

WorkoutGameMesh dropModel(double difficulty)
{
    WorkoutGameMesh mesh;
    const WorkoutGameFeatureGeometryProfile profile =
            WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::Drop, difficulty);
    std::vector<SculptedStripSample> approach;
    const double approachForwards[] = {
        profile.startMeters, -7.0, -4.0, -2.0, -1.0, 0.0
    };
    constexpr std::size_t ApproachCount =
            sizeof(approachForwards) / sizeof(approachForwards[0]);
    for (std::size_t index = 0; index < ApproachCount; ++index) {
        const double forward = approachForwards[index];
        const double progress = double(index)
                / double(ApproachCount - 1u);
        approach.push_back({
            forward, 0.0,
            1.08 + 0.15 * std::sin(Pi * progress)
                + 0.04 * std::sin(7.0 * Pi * progress),
            0.035 * std::sin(3.0 * Pi * progress)
        });
    }
    std::vector<SculptedStripSample> landing;
    const double landingForwards[] = {
        profile.landingStartMeters, 2.5, 3.75,
        profile.recoveryStartMeters, 6.0, 7.0, 8.0, 9.0,
        10.0, 11.0, profile.endMeters
    };
    constexpr std::size_t LandingCount =
            sizeof(landingForwards) / sizeof(landingForwards[0]);
    for (std::size_t index = 0; index < LandingCount; ++index) {
        const double forward = landingForwards[index];
        const double progress = double(index)
                / double(LandingCount - 1u);
        landing.push_back({
            forward, profile.surfaceOffset(forward),
            1.12 + 0.12 * std::sin(Pi * progress)
                + 0.035 * std::sin(5.0 * Pi * progress),
            0.025 * std::sin(3.0 * Pi * progress)
        });
    }
    addSculptedStrip(
            mesh, approach, 0.25,
            WorkoutGameMeshMaterial::RockTop,
            WorkoutGameMeshMaterial::RockHighlight,
            WorkoutGameMeshMaterial::DropFace);
    addSculptedStrip(
            mesh, landing, 0.25,
            WorkoutGameMeshMaterial::RockTop,
            WorkoutGameMeshMaterial::RockHighlight,
            WorkoutGameMeshMaterial::DropFace);
    addBox(mesh, -0.06, 0.0, profile.heightMeters * 0.5,
           0.06, 1.25, -profile.heightMeters * 0.5,
           WorkoutGameMeshMaterial::DropFace,
           WorkoutGameMeshMaterial::RockHighlight);
    mesh.colliders.push_back({
        0.0,
        0.0, profile.heightMeters * 0.5,
        0.12, 1.25, -profile.heightMeters * 0.5
    });
    mesh.lengthMeters = profile.endMeters - profile.startMeters;
    mesh.entry = {profile.startMeters, 1.25, 0.0};
    mesh.exit = {profile.endMeters, 1.25, 0.0};
    mesh.ready = true;
    return mesh;
}

}

WorkoutGameMesh WorkoutGameMeshLibrary::feature(
        WorkoutGameTerrainKind terrain,
        double requestedDifficulty)
{
    const double difficulty = std::clamp(
            std::isfinite(requestedDifficulty) ? requestedDifficulty : 0.0,
            0.0, 1.0);
    switch (terrain) {
    case WorkoutGameTerrainKind::Rollers:
        return rollersModel(difficulty);
    case WorkoutGameTerrainKind::Climb:
        return climbModel(difficulty);
    case WorkoutGameTerrainKind::BunnyHop:
        return bunnyHopModel(difficulty);
    case WorkoutGameTerrainKind::LogOver:
        return logModel(difficulty);
    case WorkoutGameTerrainKind::Roots:
    case WorkoutGameTerrainKind::RockGarden:
        return roughModel(terrain, difficulty);
    case WorkoutGameTerrainKind::Tabletop:
        return tabletopModel(difficulty);
    case WorkoutGameTerrainKind::RockSlab:
        return rockSlabModel(difficulty);
    case WorkoutGameTerrainKind::Drop:
        return dropModel(difficulty);
    case WorkoutGameTerrainKind::Skinny:
        return skinnyModel(difficulty);
    case WorkoutGameTerrainKind::Berm:
        return bermModel(difficulty);
    case WorkoutGameTerrainKind::SmoothTrail:
        return {};
    }
    return {};
}

WorkoutGameMesh WorkoutGameMeshLibrary::trailTile(
        double lengthMeters,
        double entryHalfWidthMeters,
        double exitHalfWidthMeters,
        double riseMeters)
{
    WorkoutGameMesh mesh;
    if (!std::isfinite(lengthMeters) || !std::isfinite(entryHalfWidthMeters)
            || !std::isfinite(exitHalfWidthMeters) || !std::isfinite(riseMeters)
            || lengthMeters <= 0.0 || entryHalfWidthMeters <= 0.0
            || exitHalfWidthMeters <= 0.0) {
        return mesh;
    }
    const double thickness = 0.16;
    const std::uint32_t a = addVertex(
            mesh, 0.0, -entryHalfWidthMeters, 0.0, 0.0, 0.0);
    const std::uint32_t b = addVertex(
            mesh, 0.0, entryHalfWidthMeters, 0.0, 0.0, 1.0);
    const std::uint32_t c = addVertex(
            mesh, lengthMeters, exitHalfWidthMeters, riseMeters, 1.0, 1.0);
    const std::uint32_t d = addVertex(
            mesh, lengthMeters, -exitHalfWidthMeters, riseMeters, 1.0, 0.0);
    const std::uint32_t e = addVertex(
            mesh, 0.0, -entryHalfWidthMeters, -thickness, 0.0, 0.0);
    const std::uint32_t f = addVertex(
            mesh, 0.0, entryHalfWidthMeters, -thickness, 0.0, 1.0);
    const std::uint32_t g = addVertex(
            mesh, lengthMeters, exitHalfWidthMeters,
            riseMeters - thickness, 1.0, 1.0);
    const std::uint32_t h = addVertex(
            mesh, lengthMeters, -exitHalfWidthMeters,
            riseMeters - thickness, 1.0, 0.0);
    addQuad(mesh, a, b, c, d, WorkoutGameMeshMaterial::Dirt);
    addQuad(mesh, e, a, d, h, WorkoutGameMeshMaterial::DirtEdge);
    addQuad(mesh, b, f, g, c, WorkoutGameMeshMaterial::DirtEdge);
    mesh.colliders.push_back({
        lengthMeters * 0.5, 0.0, riseMeters * 0.5 - thickness * 0.5,
        lengthMeters * 0.5,
        std::max(entryHalfWidthMeters, exitHalfWidthMeters),
        std::abs(riseMeters) * 0.5 + thickness * 0.5
    });
    mesh.lengthMeters = lengthMeters;
    mesh.entry = {0.0, entryHalfWidthMeters, 0.0};
    mesh.exit = {lengthMeters, exitHalfWidthMeters, riseMeters};
    mesh.ready = true;
    return mesh;
}

WorkoutGameMesh WorkoutGameMeshLibrary::bypassRibbon(
        double lengthMeters,
        double lateralMeters,
        double halfWidthMeters,
        double requestedEntryConnectorHalfWidthMeters,
        double requestedExitConnectorHalfWidthMeters)
{
    WorkoutGameMesh mesh;
    if (!std::isfinite(lengthMeters) || !std::isfinite(lateralMeters)
            || !std::isfinite(halfWidthMeters) || lengthMeters <= 0.0
            || halfWidthMeters <= 0.0) {
        return mesh;
    }
    constexpr int Samples = 20;
    const double edgeWidth = std::min(0.08, halfWidthMeters * 0.22);
    const double entryConnectorHalfWidthMeters =
            requestedEntryConnectorHalfWidthMeters > edgeWidth
            ? requestedEntryConnectorHalfWidthMeters
            : halfWidthMeters + edgeWidth;
    const double exitConnectorHalfWidthMeters =
            requestedExitConnectorHalfWidthMeters > edgeWidth
            ? requestedExitConnectorHalfWidthMeters
            : halfWidthMeters + edgeWidth;
    for (int index = 0; index <= Samples; ++index) {
        const double progress = double(index) / double(Samples);
        const double pulse = WorkoutGameTrailBranch::blend(progress);
        const double center = lateralMeters * pulse;
        const double connectorHalfWidth = entryConnectorHalfWidthMeters
                + (exitConnectorHalfWidthMeters
                   - entryConnectorHalfWidthMeters) * smoothStep(progress);
        const double localHalfWidth = (connectorHalfWidth - edgeWidth)
                + (halfWidthMeters - (connectorHalfWidth - edgeWidth))
                    * pulse;
        addVertex(mesh, lengthMeters * progress,
                  center - localHalfWidth - edgeWidth,
                  0.012, progress, 0.0);
        addVertex(mesh, lengthMeters * progress,
                  center - localHalfWidth, 0.02, progress, 0.12);
        addVertex(mesh, lengthMeters * progress,
                  center + localHalfWidth, 0.02, progress, 0.88);
        addVertex(mesh, lengthMeters * progress,
                  center + localHalfWidth + edgeWidth,
                  0.012, progress, 1.0);
        if (index > 0) {
            const std::uint32_t start = std::uint32_t(index * 4);
            addQuad(mesh, start - 3, start - 2, start + 2, start + 1,
                    WorkoutGameMeshMaterial::Bypass);
            addQuad(mesh, start - 4, start - 3, start + 1, start,
                    WorkoutGameMeshMaterial::DirtEdge);
            addQuad(mesh, start - 2, start - 1, start + 3, start + 2,
                    WorkoutGameMeshMaterial::DirtEdge);
        }
    }
    mesh.lengthMeters = lengthMeters;
    mesh.entry = {0.0, entryConnectorHalfWidthMeters, 0.0};
    mesh.exit = {lengthMeters, exitConnectorHalfWidthMeters, 0.0};
    mesh.ready = true;
    return mesh;
}

bool WorkoutGameMeshLibrary::valid(const WorkoutGameMesh &mesh)
{
    if (!mesh.ready || mesh.vertices.size() < 3u || mesh.triangles.empty()
            || !std::isfinite(mesh.lengthMeters) || mesh.lengthMeters <= 0.0) {
        return false;
    }
    const auto finiteConnector = [](const WorkoutGameMeshConnector &connector) {
        return std::isfinite(connector.forwardMeters)
                && std::isfinite(connector.halfWidthMeters)
                && std::isfinite(connector.elevationMeters)
                && connector.halfWidthMeters > 0.0;
    };
    if (!finiteConnector(mesh.entry) || !finiteConnector(mesh.exit)) return false;
    for (const WorkoutGameMeshVertex &vertex : mesh.vertices) {
        if (!std::isfinite(vertex.forwardMeters)
                || !std::isfinite(vertex.rightMeters)
                || !std::isfinite(vertex.upMeters)
                || !std::isfinite(vertex.u) || !std::isfinite(vertex.v)) {
            return false;
        }
    }
    for (const WorkoutGameMeshTriangle &triangle : mesh.triangles) {
        for (std::uint32_t index : triangle.indices) {
            if (index >= mesh.vertices.size()) return false;
        }
    }
    for (const WorkoutGameCollisionBox &box : mesh.colliders) {
        if (!std::isfinite(box.forwardMeters)
                || !std::isfinite(box.rightMeters)
                || !std::isfinite(box.upMeters)
                || !std::isfinite(box.halfForwardMeters)
                || !std::isfinite(box.halfRightMeters)
                || !std::isfinite(box.halfUpMeters)
                || box.halfForwardMeters < 0.0
                || box.halfRightMeters < 0.0
                || box.halfUpMeters < 0.0) {
            return false;
        }
    }
    return true;
}

std::vector<WorkoutGameProjectedMeshTriangle>
WorkoutGameMeshProjector::project(
        const WorkoutGameMeshInstance &instance,
        const WorkoutGameRoadProjectionFrame &road)
{
    std::vector<WorkoutGameProjectedMeshTriangle> result;
    if (!WorkoutGameMeshLibrary::valid(instance.mesh)
            || !road.ready
            || !std::isfinite(instance.anchorDistanceMeters)
            || !std::isfinite(instance.lateralMeters)
            || !std::isfinite(instance.elevationMeters)
            || !std::isfinite(instance.yawDegrees)
            || !std::isfinite(instance.forwardScale)
            || !std::isfinite(instance.entryRightScale)
            || !std::isfinite(instance.exitRightScale)
            || !std::isfinite(instance.upScale)
            || !std::isfinite(instance.occlusionAllowancePixels)
            || instance.forwardScale <= 0.0
            || instance.entryRightScale <= 0.0
            || instance.exitRightScale <= 0.0
            || instance.upScale <= 0.0
            || instance.occlusionAllowancePixels < 0.0) {
        return result;
    }
    struct WorldVertex
    {
        double distance = 0.0;
        double lateral = 0.0;
        double elevation = 0.0;
        double u = 0.0;
        double v = 0.0;
    };
    struct ScreenVertex
    {
        WorkoutGameProjectedMeshVertex projected;
        double occlusionY = 0.0;
    };
    const auto interpolateWorld = [](const WorldVertex &from,
                                     const WorldVertex &to,
                                     double amount) {
        const auto value = [amount](double first, double second) {
            return first + (second - first) * amount;
        };
        return WorldVertex{
            value(from.distance, to.distance),
            value(from.lateral, to.lateral),
            value(from.elevation, to.elevation),
            value(from.u, to.u),
            value(from.v, to.v)
        };
    };
    const auto clipDistance = [&interpolateWorld](
            const std::vector<WorldVertex> &input,
            double boundary,
            bool keepGreater) {
        std::vector<WorldVertex> output;
        if (input.empty()) return output;
        const auto inside = [boundary, keepGreater](const WorldVertex &vertex) {
            return keepGreater ? vertex.distance >= boundary - 1e-9
                               : vertex.distance <= boundary + 1e-9;
        };
        WorldVertex previous = input.back();
        bool previousInside = inside(previous);
        for (const WorldVertex &current : input) {
            const bool currentInside = inside(current);
            if (currentInside != previousInside) {
                const double span = current.distance - previous.distance;
                const double amount = std::abs(span) > 1e-12
                        ? std::clamp(
                            (boundary - previous.distance) / span,
                            0.0, 1.0)
                        : 0.0;
                output.push_back(interpolateWorld(previous, current, amount));
            }
            if (currentInside) output.push_back(current);
            previous = current;
            previousInside = currentInside;
        }
        return output;
    };
    const auto interpolateScreen = [](const ScreenVertex &from,
                                      const ScreenVertex &to,
                                      double amount) {
        const auto value = [amount](double first, double second) {
            return first + (second - first) * amount;
        };
        ScreenVertex result;
        result.projected = {
            value(from.projected.x, to.projected.x),
            value(from.projected.y, to.projected.y),
            value(from.projected.depthMeters, to.projected.depthMeters),
            value(from.projected.u, to.projected.u),
            value(from.projected.v, to.projected.v)
        };
        result.occlusionY = value(from.occlusionY, to.occlusionY);
        return result;
    };
    const auto clipOcclusion = [&interpolateScreen](
            const std::vector<ScreenVertex> &input,
            double allowancePixels) {
        std::vector<ScreenVertex> output;
        if (input.empty()) return output;
        const auto visibility = [allowancePixels](const ScreenVertex &vertex) {
            return vertex.occlusionY + allowancePixels - vertex.projected.y;
        };
        ScreenVertex previous = input.back();
        double previousVisibility = visibility(previous);
        bool previousInside = previousVisibility >= -1e-6;
        for (const ScreenVertex &current : input) {
            const double currentVisibility = visibility(current);
            const bool currentInside = currentVisibility >= -1e-6;
            if (currentInside != previousInside) {
                const double span = previousVisibility - currentVisibility;
                const double amount = std::abs(span) > 1e-12
                        ? std::clamp(previousVisibility / span, 0.0, 1.0)
                        : 0.0;
                output.push_back(interpolateScreen(previous, current, amount));
            }
            if (currentInside) output.push_back(current);
            previous = current;
            previousVisibility = currentVisibility;
            previousInside = currentInside;
        }
        return output;
    };

    const double yaw = instance.yawDegrees * Pi / 180.0;
    const double cosine = std::cos(yaw);
    const double sine = std::sin(yaw);
    const double nearDistance = road.slices.back().worldDistanceMeters;
    const double farDistance = road.slices.front().worldDistanceMeters;
    result.reserve(instance.mesh.triangles.size() * 2u);
    for (const WorkoutGameMeshTriangle &source : instance.mesh.triangles) {
        std::vector<WorldVertex> world;
        world.reserve(3);
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const WorkoutGameMeshVertex &vertex =
                    instance.mesh.vertices[source.indices[corner]];
            const double localForward =
                    vertex.forwardMeters * instance.forwardScale;
            const double meshProgress = instance.mesh.lengthMeters > 0.0
                    ? std::clamp(
                        (vertex.forwardMeters
                         - instance.mesh.entry.forwardMeters)
                            / instance.mesh.lengthMeters,
                        0.0, 1.0)
                    : 0.0;
            const double rightScale = instance.entryRightScale
                    + (instance.exitRightScale
                       - instance.entryRightScale) * smoothStep(meshProgress);
            const double localRight = vertex.rightMeters * rightScale;
            const double forward = localForward * cosine - localRight * sine;
            const double right = localForward * sine + localRight * cosine;
            world.push_back({
                instance.anchorDistanceMeters + forward,
                instance.lateralMeters + right,
                instance.elevationMeters + vertex.upMeters * instance.upScale,
                vertex.u,
                vertex.v
            });
        }
        world = clipDistance(world, nearDistance, true);
        world = clipDistance(world, farDistance, false);
        if (world.size() < 3u) continue;

        std::vector<ScreenVertex> screen;
        screen.reserve(world.size());
        bool ready = true;
        for (const WorldVertex &vertex : world) {
            const WorkoutGameRoadProjectedPoint projected =
                    WorkoutGameRoadProjection::projectPoint(
                        road,
                        vertex.distance,
                        vertex.lateral,
                        vertex.elevation,
                        instance.anchorToBaseSurface);
            if (!projected.ready) {
                ready = false;
                break;
            }
            screen.push_back({{
                projected.x, projected.y, projected.depthMeters,
                vertex.u, vertex.v
            }, projected.occlusionY});
        }
        if (!ready) continue;
        if (instance.clipToRoadOcclusion) {
            screen = clipOcclusion(
                    screen, instance.occlusionAllowancePixels);
            if (screen.size() < 3u) continue;
        }
        for (std::size_t corner = 1; corner + 1 < screen.size(); ++corner) {
            WorkoutGameProjectedMeshTriangle triangle;
            triangle.material = source.material;
            triangle.vertices = {
                screen[0].projected,
                screen[corner].projected,
                screen[corner + 1].projected
            };
            triangle.depthMeters = (
                    triangle.vertices[0].depthMeters
                    + triangle.vertices[1].depthMeters
                    + triangle.vertices[2].depthMeters) / 3.0;
            result.push_back(triangle);
        }
    }
    std::stable_sort(result.begin(), result.end(),
            [](const WorkoutGameProjectedMeshTriangle &left,
               const WorkoutGameProjectedMeshTriangle &right) {
                return left.depthMeters > right.depthMeters;
            });
    return result;
}
