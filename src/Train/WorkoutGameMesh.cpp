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

#include <algorithm>
#include <cmath>

namespace {

constexpr double Pi = 3.14159265358979323846;

double smoothStep(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    return clamped * clamped * (3.0 - 2.0 * clamped);
}

double smootherStep(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    return clamped * clamped * clamped
            * (clamped * (clamped * 6.0 - 15.0) + 10.0);
}

double smoothPulse(double value)
{
    const double progress = std::clamp(value, 0.0, 1.0);
    if (progress <= 0.0 || progress >= 1.0) return 0.0;
    if (progress < 0.35) return smoothStep(progress / 0.35);
    if (progress > 0.80) return smootherStep((1.0 - progress) / 0.20);
    return 1.0;
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
        double base,
        double length,
        double width,
        double height,
        WorkoutGameMeshMaterial side,
        WorkoutGameMeshMaterial top)
{
    const double f0 = forward - length * 0.5;
    const double f1 = forward + length * 0.5;
    const double r0 = right - width * 0.5;
    const double r1 = right + width * 0.5;
    const double u0 = base;
    const double u1 = base + height;
    const std::uint32_t start = std::uint32_t(mesh.vertices.size());
    addVertex(mesh, f0, r0, u0, 0.0, 1.0);
    addVertex(mesh, f0, r1, u0, 1.0, 1.0);
    addVertex(mesh, f1, r1, u0, 1.0, 0.0);
    addVertex(mesh, f1, r0, u0, 0.0, 0.0);
    addVertex(mesh, f0, r0, u1, 0.0, 1.0);
    addVertex(mesh, f0, r1, u1, 1.0, 1.0);
    addVertex(mesh, f1, r1, u1, 1.0, 0.0);
    addVertex(mesh, f1, r0, u1, 0.0, 0.0);
    addQuad(mesh, start + 4, start + 5, start + 6, start + 7, top);
    addQuad(mesh, start + 0, start + 4, start + 7, start + 3, side);
    addQuad(mesh, start + 1, start + 2, start + 6, start + 5, side);
    addQuad(mesh, start + 0, start + 1, start + 5, start + 4, side);
    addQuad(mesh, start + 3, start + 7, start + 6, start + 2, side);
}

WorkoutGameMesh logModel(double difficulty)
{
    WorkoutGameMesh mesh;
    const WorkoutGameFeatureGeometryProfile profile =
            WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::LogOver, difficulty);
    const double radius = profile.heightMeters * 0.5;
    const double halfWidth = 1.55;
    constexpr int Segments = WorkoutGameLogRadialSegments;
    for (int side = 0; side < 2; ++side) {
        const double right = side == 0 ? -halfWidth : halfWidth;
        for (int index = 0; index < Segments; ++index) {
            const double angle = 2.0 * Pi * double(index) / double(Segments);
            addVertex(mesh,
                    std::cos(angle) * radius,
                    right,
                    radius + std::sin(angle) * radius,
                    double(side), double(index) / double(Segments));
        }
    }
    for (int index = 0; index < Segments; ++index) {
        const int next = (index + 1) % Segments;
        addQuad(mesh,
                std::uint32_t(index), std::uint32_t(next),
                std::uint32_t(Segments + next),
                std::uint32_t(Segments + index),
                index < 3 ? WorkoutGameMeshMaterial::WoodTop
                          : WorkoutGameMeshMaterial::WoodSide);
    }
    mesh.colliders.push_back({
        0.0, 0.0, radius,
        radius, halfWidth, radius
    });
    mesh.lengthMeters = radius * 2.0;
    mesh.entry = {-radius, halfWidth, 0.0};
    mesh.exit = {radius, halfWidth, 0.0};
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
    const double halfWidth = 1.35;
    const double forward[] = {
        profile.startMeters, profile.plateauStartMeters,
        profile.plateauEndMeters, profile.endMeters
    };
    const double up[] = {0.0, height, height, 0.0};
    for (int section = 0; section < 4; ++section) {
        addVertex(mesh, forward[section], -halfWidth, up[section],
                  double(section) / 3.0, 0.0);
        addVertex(mesh, forward[section], halfWidth, up[section],
                  double(section) / 3.0, 1.0);
    }
    for (std::uint32_t section = 0; section < 3; ++section) {
        addQuad(mesh,
                section * 2, section * 2 + 1,
                section * 2 + 3, section * 2 + 2,
                WorkoutGameMeshMaterial::Dirt);
    }
    addQuad(mesh, 0, 2, 4, 6, WorkoutGameMeshMaterial::DirtEdge);
    addQuad(mesh, 1, 7, 5, 3, WorkoutGameMeshMaterial::DirtEdge);
    mesh.colliders.push_back({
        0.0, 0.0, height * 0.5,
        3.2, halfWidth, height * 0.5
    });
    mesh.lengthMeters = profile.endMeters - profile.startMeters;
    mesh.entry = {profile.startMeters, halfWidth, 0.0};
    mesh.exit = {profile.endMeters, halfWidth, 0.0};
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
    const double halfWidth = 1.30;
    const double forward[] = {
        profile.startMeters, profile.plateauStartMeters,
        profile.plateauEndMeters, profile.endMeters
    };
    const double up[] = {0.0, height, height, 0.0};
    for (int section = 0; section < 4; ++section) {
        addVertex(mesh, forward[section], -halfWidth, up[section],
                  double(section) / 3.0, 0.0);
        addVertex(mesh, forward[section], halfWidth, up[section],
                  double(section) / 3.0, 1.0);
    }
    for (std::uint32_t section = 0; section < 3; ++section) {
        addQuad(mesh,
                section * 2, section * 2 + 1,
                section * 2 + 3, section * 2 + 2,
                WorkoutGameMeshMaterial::RockTop);
    }
    addQuad(mesh, 0, 2, 4, 6, WorkoutGameMeshMaterial::RockSide);
    addQuad(mesh, 1, 7, 5, 3, WorkoutGameMeshMaterial::RockSide);
    mesh.colliders.push_back({
        1.5, 0.0, height * 0.5,
        4.5, halfWidth, height * 0.5
    });
    mesh.lengthMeters = profile.endMeters - profile.startMeters;
    mesh.entry = {profile.startMeters, halfWidth, 0.0};
    mesh.exit = {profile.endMeters, halfWidth, 0.0};
    mesh.ready = true;
    return mesh;
}

WorkoutGameMesh roughModel(
        WorkoutGameTerrainKind terrain,
        double difficulty)
{
    WorkoutGameMesh mesh;
    if (terrain == WorkoutGameTerrainKind::Roots) {
        for (int index = -2; index <= 2; ++index) {
            const double height = 0.08 + 0.04 * ((index + 2) % 2)
                    + 0.04 * difficulty;
            addBox(mesh, double(index) * 0.65, 0.0, 0.0,
                   0.14, 2.7 - 0.12 * std::abs(index), height,
                   WorkoutGameMeshMaterial::WoodSide,
                   WorkoutGameMeshMaterial::WoodTop);
            mesh.colliders.push_back({
                double(index) * 0.65, 0.0, height * 0.5,
                0.07, 1.25, height * 0.5
            });
        }
        mesh.lengthMeters = 3.0;
    } else {
        for (int index = 0; index < 7; ++index) {
            const double forward = -2.2 + double(index) * 0.72;
            const double right = ((index * 5) % 7 - 3) * 0.32;
            const double size = 0.28 + 0.08 * double(index % 3)
                    + 0.10 * difficulty;
            addBox(mesh, forward, right, 0.0,
                   size * 1.2, size * 1.5, size,
                   WorkoutGameMeshMaterial::RockSide,
                   WorkoutGameMeshMaterial::RockTop);
            mesh.colliders.push_back({
                forward, right, size * 0.5,
                size * 0.6, size * 0.75, size * 0.5
            });
        }
        mesh.lengthMeters = 5.0;
    }
    mesh.entry = {-mesh.lengthMeters * 0.5, 1.4, 0.0};
    mesh.exit = {mesh.lengthMeters * 0.5, 1.4, 0.0};
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
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver:
        return logModel(difficulty);
    case WorkoutGameTerrainKind::Roots:
    case WorkoutGameTerrainKind::RockGarden:
        return roughModel(terrain, difficulty);
    case WorkoutGameTerrainKind::Tabletop:
        return tabletopModel(difficulty);
    case WorkoutGameTerrainKind::RockSlab:
        return rockSlabModel(difficulty);
    case WorkoutGameTerrainKind::Drop: {
        WorkoutGameMesh mesh;
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(terrain, difficulty);
        const double halfWidth = 1.5;
        const double forward[] = {
            profile.startMeters, profile.plateauStartMeters,
            profile.plateauEndMeters, profile.endMeters
        };
        const double up[] = {
            0.0, profile.heightMeters, profile.heightMeters, 0.0
        };
        for (int section = 0; section < 4; ++section) {
            addVertex(mesh, forward[section], -halfWidth, up[section],
                      double(section) / 3.0, 0.0);
            addVertex(mesh, forward[section], halfWidth, up[section],
                      double(section) / 3.0, 1.0);
        }
        for (std::uint32_t section = 0; section < 3; ++section) {
            addQuad(mesh, section * 2, section * 2 + 1,
                    section * 2 + 3, section * 2 + 2,
                    section == 0 ? WorkoutGameMeshMaterial::DropFace
                                 : WorkoutGameMeshMaterial::RockTop);
        }
        mesh.colliders.push_back({
            (profile.plateauStartMeters + profile.plateauEndMeters) * 0.5,
            0.0, profile.heightMeters * 0.5,
            (profile.plateauEndMeters - profile.plateauStartMeters) * 0.5,
            halfWidth, -profile.heightMeters * 0.5
        });
        mesh.lengthMeters = profile.endMeters - profile.startMeters;
        mesh.entry = {profile.startMeters, halfWidth, 0.0};
        mesh.exit = {profile.endMeters, halfWidth, 0.0};
        mesh.ready = true;
        return mesh;
    }
    default: {
        WorkoutGameMesh mesh;
        addBox(mesh, 0.0, 0.0, 0.0, 1.0, 2.4, 0.15,
               WorkoutGameMeshMaterial::DirtEdge,
               WorkoutGameMeshMaterial::Dirt);
        mesh.colliders.push_back({0.0, 0.0, 0.075, 0.5, 1.2, 0.075});
        mesh.lengthMeters = 1.0;
        mesh.entry = {-0.5, 1.2, 0.0};
        mesh.exit = {0.5, 1.2, 0.0};
        mesh.ready = true;
        return mesh;
    }
    }
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
        double halfWidthMeters)
{
    WorkoutGameMesh mesh;
    if (!std::isfinite(lengthMeters) || !std::isfinite(lateralMeters)
            || !std::isfinite(halfWidthMeters) || lengthMeters <= 0.0
            || halfWidthMeters <= 0.0) {
        return mesh;
    }
    constexpr int Samples = 20;
    for (int index = 0; index <= Samples; ++index) {
        const double progress = double(index) / double(Samples);
        const double center = lateralMeters * smoothPulse(progress);
        addVertex(mesh, lengthMeters * progress,
                  center - halfWidthMeters, 0.02, progress, 0.0);
        addVertex(mesh, lengthMeters * progress,
                  center + halfWidthMeters, 0.02, progress, 1.0);
        if (index > 0) {
            const std::uint32_t start = std::uint32_t(index * 2);
            addQuad(mesh, start - 2, start - 1, start + 1, start,
                    WorkoutGameMeshMaterial::Bypass);
        }
    }
    mesh.lengthMeters = lengthMeters;
    mesh.entry = {0.0, halfWidthMeters, 0.0};
    mesh.exit = {lengthMeters, halfWidthMeters, 0.0};
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
            || !std::isfinite(instance.rightScale)
            || !std::isfinite(instance.upScale)
            || instance.forwardScale <= 0.0
            || instance.rightScale <= 0.0
            || instance.upScale <= 0.0) {
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
            const std::vector<ScreenVertex> &input) {
        std::vector<ScreenVertex> output;
        if (input.empty()) return output;
        const auto visibility = [](const ScreenVertex &vertex) {
            return vertex.occlusionY - vertex.projected.y;
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
            const double localRight =
                    vertex.rightMeters * instance.rightScale;
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
        screen = clipOcclusion(screen);
        if (screen.size() < 3u) continue;
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
