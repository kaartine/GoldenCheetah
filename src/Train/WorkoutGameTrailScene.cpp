/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameTrailScene.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

constexpr double BaseCenterY = 0.61;
constexpr double MinimumCenterY = 0.2;
constexpr double MaximumCenterY = 0.88;
constexpr double ElevationScale = 0.035;

std::uint64_t mixed(std::uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

double unitValue(std::uint64_t value)
{
    return double(mixed(value) >> 11) * (1.0 / 9007199254740992.0);
}

double trailCurve(double distanceMeters, std::uint32_t seed)
{
    const double phase = double(seed % 4093u) / 4093.0 * 6.283185307179586;
    return 0.036 * std::sin(distanceMeters * 0.075 + phase)
            + 0.014 * std::sin(distanceMeters * 0.19 + phase * 0.37);
}

double halfWidth(
        WorkoutGameTerrainKind terrain,
        double distanceMeters,
        std::uint32_t seed)
{
    double base = 0.092;
    switch (terrain) {
    case WorkoutGameTerrainKind::Skinny: base = 0.066; break;
    case WorkoutGameTerrainKind::Berm: base = 0.108; break;
    case WorkoutGameTerrainKind::RockGarden: base = 0.1; break;
    default: break;
    }
    const double phase = distanceMeters * 0.11 + double(seed % 97u) * 0.03;
    return base * (1.0 + 0.08 * std::sin(phase));
}

WorkoutGameTrailPoint pointAt(
        const WorkoutGameWorldSnapshot &world,
        double distanceMeters)
{
    const double riderDistance = world.rider.distanceMeters;
    const double localDistance = distanceMeters + world.terrainOffsetMeters;
    const double riderLocalDistance = riderDistance + world.terrainOffsetMeters;
    const double elevation = WorkoutGamePhysics::terrainHeight(
            world.terrain,
            localDistance,
            world.gradePercent,
            world.difficulty,
            world.seed);
    const double riderElevation = WorkoutGamePhysics::terrainHeight(
            world.terrain,
            riderLocalDistance,
            world.gradePercent,
            world.difficulty,
            world.seed);
    const double width = halfWidth(world.terrain, distanceMeters, world.seed);
    const double curve = trailCurve(distanceMeters, world.seed)
            - trailCurve(riderDistance, world.seed);
    const double unclampedCenter = BaseCenterY
            - (elevation - riderElevation) * ElevationScale + curve;
    const double center = std::clamp(
            unclampedCenter,
            MinimumCenterY + width,
            MaximumCenterY - width);

    WorkoutGameTrailPoint point;
    point.worldDistanceMeters = distanceMeters;
    point.xNormalized = std::clamp(
            WorkoutGameTrailScene::RiderXNormalized
                + (distanceMeters - riderDistance)
                    / WorkoutGameTrailScene::VisibleMeters,
            0.0,
            1.0);
    point.centerYNormalized = center;
    point.farEdgeYNormalized = center - width;
    point.nearEdgeYNormalized = center + width;
    return point;
}

double spacingFor(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::Roots: return 3.6;
    case WorkoutGameTerrainKind::Rollers: return 6.0;
    case WorkoutGameTerrainKind::Climb: return 7.0;
    case WorkoutGameTerrainKind::RockGarden: return 4.1;
    case WorkoutGameTerrainKind::BunnyHop: return 11.0;
    case WorkoutGameTerrainKind::Drop: return 13.0;
    case WorkoutGameTerrainKind::Skinny: return 5.0;
    case WorkoutGameTerrainKind::Berm: return 9.0;
    case WorkoutGameTerrainKind::SmoothTrail: return 7.5;
    }
    return 7.5;
}

WorkoutGameTrailPropKind propKindFor(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::Roots: return WorkoutGameTrailPropKind::Root;
    case WorkoutGameTerrainKind::Rollers:
        return WorkoutGameTrailPropKind::RollerMarker;
    case WorkoutGameTerrainKind::Climb:
        return WorkoutGameTrailPropKind::ClimbMarker;
    case WorkoutGameTerrainKind::RockGarden:
        return WorkoutGameTrailPropKind::Rock;
    case WorkoutGameTerrainKind::BunnyHop: return WorkoutGameTrailPropKind::Log;
    case WorkoutGameTerrainKind::Drop:
        return WorkoutGameTrailPropKind::DropMarker;
    case WorkoutGameTerrainKind::Skinny: return WorkoutGameTrailPropKind::Plank;
    case WorkoutGameTerrainKind::Berm:
        return WorkoutGameTrailPropKind::BermMarker;
    case WorkoutGameTerrainKind::SmoothTrail:
        return WorkoutGameTrailPropKind::Pebble;
    }
    return WorkoutGameTrailPropKind::Pebble;
}

}

WorkoutGameTrailSceneSnapshot WorkoutGameTrailScene::build(
        const WorkoutGameWorldSnapshot &world,
        int requestedSampleCount)
{
    WorkoutGameTrailSceneSnapshot result;
    if (!world.ready
            || !std::isfinite(world.rider.distanceMeters)
            || !std::isfinite(world.terrainOffsetMeters)
            || !std::isfinite(world.gradePercent)
            || !std::isfinite(world.difficulty)) {
        return result;
    }

    const int sampleCount = std::clamp(requestedSampleCount, 16, 256);
    const double visibleStart = world.rider.distanceMeters
            - RiderXNormalized * VisibleMeters;
    const double visibleEnd = visibleStart + VisibleMeters;
    result.points.reserve(std::size_t(sampleCount + 1));
    for (int index = 0; index <= sampleCount; ++index) {
        const double amount = double(index) / double(sampleCount);
        result.points.push_back(pointAt(
                world, visibleStart + VisibleMeters * amount));
    }

    const WorkoutGameTrailPoint rider = pointAt(
            world, world.rider.distanceMeters);
    result.riderXNormalized = rider.xNormalized;
    result.riderYNormalized = rider.centerYNormalized;

    const double spacing = spacingFor(world.terrain);
    const std::int64_t firstIndex = std::int64_t(
            std::floor(visibleStart / spacing)) - 1;
    const std::int64_t lastIndex = std::int64_t(
            std::ceil(visibleEnd / spacing)) + 1;
    for (std::int64_t index = firstIndex; index <= lastIndex; ++index) {
        const std::uint64_t identity = std::uint64_t(index)
                ^ (std::uint64_t(world.seed) << 32)
                ^ (std::uint64_t(world.terrain) << 56);
        const double jitter = (unitValue(identity) - 0.5) * spacing * 0.36;
        const double distance = double(index) * spacing + jitter;
        if (distance < visibleStart || distance > visibleEnd) continue;

        const WorkoutGameTrailPoint point = pointAt(world, distance);
        const double lateral = (unitValue(identity ^ 0xd1b54a32d192ed03ULL)
                * 2.0 - 1.0) * 0.72;
        WorkoutGameTrailProp prop;
        prop.kind = propKindFor(world.terrain);
        prop.variant = std::uint32_t(mixed(identity));
        prop.worldDistanceMeters = distance;
        prop.lateralPosition = lateral;
        prop.xNormalized = point.xNormalized;
        prop.farEdgeYNormalized = point.farEdgeYNormalized;
        prop.nearEdgeYNormalized = point.nearEdgeYNormalized;
        prop.yNormalized = point.centerYNormalized
                + lateral * (point.nearEdgeYNormalized
                    - point.centerYNormalized) * 0.82;
        prop.scale = 0.78
                + 0.18 * ((lateral + 1.0) * 0.5)
                + 0.08 * unitValue(identity ^ 0x94d049bb133111ebULL);
        prop.depthKey = prop.yNormalized;
        result.props.push_back(prop);
    }
    std::stable_sort(
            result.props.begin(), result.props.end(),
            [](const WorkoutGameTrailProp &left,
               const WorkoutGameTrailProp &right) {
                if (left.depthKey != right.depthKey) {
                    return left.depthKey < right.depthKey;
                }
                return left.xNormalized < right.xNormalized;
            });

    result.ready = true;
    return result;
}
