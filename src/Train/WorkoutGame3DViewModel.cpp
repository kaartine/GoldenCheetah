/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DViewModel.h"

#include <QByteArray>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

constexpr double Pi = 3.14159265358979323846;
constexpr double FloorBucketMeters = 10.0;
constexpr double FloorBehindMeters = 15.0;
constexpr double FloorAheadMeters = 130.0;
constexpr double FeatureBucketMeters = 12.0;
constexpr double FeatureBehindMeters = 15.0;
constexpr double FeatureAheadMeters = 180.0;
constexpr int MaximumVisibleFeatures = 32;
constexpr double TreeSpacingMeters = 6.0;

double finiteOrZero(double value)
{
    return std::isfinite(value) ? value : 0.0;
}

std::uint32_t mix(std::uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

}

WorkoutGame3DViewModel::WorkoutGame3DViewModel(QObject *parent) :
    QObject(parent),
    trail(std::make_unique<WorkoutGame3DGeometry>(
            WorkoutGame3DGeometry::Layer::Trail))
{
    for (std::unique_ptr<WorkoutGame3DGeometry> &buffer : floorBuffers) {
        buffer = std::make_unique<WorkoutGame3DGeometry>(
                WorkoutGame3DGeometry::Layer::ForestFloor);
    }
    const QByteArray requested = qgetenv("GC_WORKOUT_GAME_3D_CAMERA")
            .trimmed().toLower();
    if (requested == "low-centre") {
        currentCameraComposition = QStringLiteral("low-centre");
        cameraBackDistanceMeters = 7.4;
        cameraHeightDistanceMeters = 2.55;
        cameraLookAheadDistanceMeters = 11.0;
        cameraTargetHeightDistanceMeters = 0.75;
    } else if (requested == "shoulder") {
        currentCameraComposition = QStringLiteral("shoulder");
        cameraSideDistanceMeters = 0.65;
    } else {
        currentCameraComposition = QStringLiteral("medium-centre");
    }
}

WorkoutGame3DViewModel::~WorkoutGame3DViewModel() = default;

void WorkoutGame3DViewModel::setCourse(
        const WorkoutGameCourse &course,
        double ftpWatts)
{
    roadCourse = WorkoutGameRoadCourseBuilder::build(course, ftpWatts);
    trail->setCourse(roadCourse);
    floorBucket = std::numeric_limits<int>::min();
    featureBucket = std::numeric_limits<int>::min();
    treeBucket = std::numeric_limits<int>::min();
    visibleTrees.clear();
    rebuildFloor(0.0);
    sceneReady = roadCourse.ready && trail->ready()
            && floorBuffers[std::size_t(activeFloorBuffer)]->ready();
    rebuildFeatures(0.0);
    rebuildTrees(0.0);
    emit courseChanged();
    emit sceneChanged();
}

void WorkoutGame3DViewModel::setFrame(
        const WorkoutGameVisualSnapshot &frame,
        double watts,
        double targetWatts,
        int cadenceRpm,
        int heartRate,
        int virtualGear)
{
    setTelemetry(
            watts, targetWatts, cadenceRpm, heartRate, virtualGear);
    if (!sceneReady || !frame.world.ready) return;

    const double distanceMeters = std::clamp(
            finiteOrZero(frame.world.rider.distanceMeters),
            0.0, roadCourse.totalLengthMeters);

    const WorkoutGameRoadSample sample = WorkoutGameRoadCourseBuilder::sample(
            roadCourse, distanceMeters);
    if (!sample.ready) return;
    const double lateral = finiteOrZero(frame.feature.lateralOffsetMeters);
    const double rightX = std::cos(sample.center.headingRadians);
    const double rightZ = -std::sin(sample.center.headingRadians);
    riderPositionX = sample.center.xMeters + lateral * rightX;
    cameraGroundY = sample.center.elevationMeters
            - sample.nonPhysicalFeatureOffsetMeters;
    const double physicsAir = std::max(
            0.0, finiteOrZero(frame.world.rider.airHeightMeters()));
    const double featureAir = frame.feature.ready
            && frame.feature.route != WorkoutGameRoute::SafeBypass
            && frame.feature.outcome == WorkoutGameFeatureOutcome::Completed
            ? std::max(0.0,
                finiteOrZero(frame.feature.verticalOffsetMeters))
            : 0.0;
    const double visualGround = sample.center.elevationMeters
            - (frame.feature.route == WorkoutGameRoute::SafeBypass
                ? sample.nonPhysicalFeatureOffsetMeters : 0.0);
    riderPositionY = visualGround + std::max(physicsAir, featureAir);
    riderPositionZ = sample.center.zMeters + lateral * rightZ;
    riderHeadingDegrees = sample.center.headingRadians * 180.0 / Pi;
    riderPitchDegrees = finiteOrZero(frame.world.rider.pitchDegrees)
            + finiteOrZero(frame.feature.pitchDegrees);
    riderRollDegrees = finiteOrZero(frame.world.rider.rollDegrees);
    currentPedalAngle = std::fmod(
            finiteOrZero(frame.riderPedalCycles) * 360.0, 360.0);
    currentSpeedKph = std::max(0.0, finiteOrZero(frame.simulation.speedKph));
    currentDistanceMeters = distanceMeters;
    currentWorkoutTimeSeconds = int(std::clamp<std::int64_t>(
            frame.simulation.workoutTimeMs / 1000, 0, 24 * 60 * 60));
    currentTerrainName = terrainText(frame.world.terrain);
    currentFeatureStatus = featureText(frame.feature);
    currentReadinessPercent = int(std::lround(std::clamp(
            finiteOrZero(frame.feature.readiness), 0.0, 1.0) * 100.0));
    rebuildFloor(currentDistanceMeters);
    rebuildFeatures(currentDistanceMeters);
    rebuildTrees(currentDistanceMeters);
    emit sceneChanged();
}

void WorkoutGame3DViewModel::setTelemetry(
        double watts,
        double targetWatts,
        int cadenceRpm,
        int heartRate,
        int virtualGear)
{
    currentWatts = std::max(0.0, finiteOrZero(watts));
    currentTargetWatts = std::max(0.0, finiteOrZero(targetWatts));
    currentCadenceRpm = std::clamp(cadenceRpm, 0, 300);
    currentHeartRate = std::clamp(heartRate, 0, 300);
    currentVirtualGear = std::max(1, virtualGear);
    emit telemetryChanged();
}

void WorkoutGame3DViewModel::setFps(double value)
{
    const double normalized = std::max(0.0, finiteOrZero(value));
    if (std::abs(normalized - currentFps) < 0.05) return;
    currentFps = normalized;
    emit fpsChanged();
}

QString WorkoutGame3DViewModel::terrainText(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::SmoothTrail: return tr("Singletrack");
    case WorkoutGameTerrainKind::Roots: return tr("Roots");
    case WorkoutGameTerrainKind::Rollers: return tr("Rollers");
    case WorkoutGameTerrainKind::Climb: return tr("Climb");
    case WorkoutGameTerrainKind::RockGarden: return tr("Rock garden");
    case WorkoutGameTerrainKind::BunnyHop: return tr("Bunny hop");
    case WorkoutGameTerrainKind::Drop: return tr("Drop");
    case WorkoutGameTerrainKind::Skinny: return tr("Skinny");
    case WorkoutGameTerrainKind::Berm: return tr("Berm");
    case WorkoutGameTerrainKind::LogOver: return tr("Log over");
    case WorkoutGameTerrainKind::Tabletop: return tr("Tabletop");
    case WorkoutGameTerrainKind::RockSlab: return tr("Rock slab");
    }
    return tr("Trail");
}

QString WorkoutGame3DViewModel::featureText(
        const WorkoutGameFeatureRuntimeSnapshot &feature)
{
    if (!feature.ready || feature.phase == WorkoutGameFeaturePhase::None) {
        return QString();
    }
    const QString name = terrainText(feature.terrain);
    switch (feature.phase) {
    case WorkoutGameFeaturePhase::Approach:
        return tr("%1 ahead").arg(name);
    case WorkoutGameFeaturePhase::Measure:
        return tr("Match target for %1").arg(name);
    case WorkoutGameFeaturePhase::Committed:
        return tr("Line committed: %1").arg(name);
    case WorkoutGameFeaturePhase::Action:
        return name;
    case WorkoutGameFeaturePhase::Recovery:
        return feature.outcome == WorkoutGameFeatureOutcome::Completed
                ? tr("%1 completed").arg(name)
                : tr("%1 bypassed").arg(name);
    case WorkoutGameFeaturePhase::None:
        break;
    }
    return QString();
}

void WorkoutGame3DViewModel::rebuildFeatures(double distanceMeters)
{
    const int bucket = int(std::floor(
            distanceMeters / FeatureBucketMeters));
    if (bucket == featureBucket) return;
    featureBucket = bucket;
    courseFeatures.clear();
    if (!roadCourse.ready) return;
    const double minimumDistance = std::max(
            0.0, distanceMeters - FeatureBehindMeters);
    const double maximumDistance = std::min(
            roadCourse.totalLengthMeters,
            distanceMeters + FeatureAheadMeters);
    for (const WorkoutGameRoadPiece &piece : roadCourse.pieces) {
        if (!piece.challenge.enabled) continue;
        if (piece.challenge.obstacleDistanceMeters < minimumDistance
                || piece.challenge.obstacleDistanceMeters > maximumDistance) {
            continue;
        }
        const WorkoutGameRoadSample sample = WorkoutGameRoadCourseBuilder::sample(
                roadCourse, piece.challenge.obstacleDistanceMeters);
        if (!sample.ready) continue;
        QVariantMap feature;
        feature.insert(QStringLiteral("kind"), int(piece.terrain));
        feature.insert(QStringLiteral("x"), sample.center.xMeters);
        feature.insert(QStringLiteral("y"), sample.center.elevationMeters
                - sample.nonPhysicalFeatureOffsetMeters);
        feature.insert(QStringLiteral("z"), sample.center.zMeters);
        feature.insert(QStringLiteral("yaw"),
                       sample.center.headingRadians * 180.0 / Pi);
        feature.insert(QStringLiteral("difficulty"), piece.difficulty);
        courseFeatures.push_back(feature);
        if (courseFeatures.size() >= MaximumVisibleFeatures) break;
    }
    emit courseChanged();
}

void WorkoutGame3DViewModel::rebuildFloor(double distanceMeters)
{
    if (!roadCourse.ready) {
        floorBuffers[0]->setCourse(roadCourse);
        floorBuffers[1]->setCourse(roadCourse);
        activeFloorBuffer = 0;
        emit floorGeometryChanged();
        return;
    }
    const int bucket = int(std::floor(distanceMeters / FloorBucketMeters));
    if (bucket == floorBucket) return;
    floorBucket = bucket;
    const double start = std::max(
            0.0, distanceMeters - FloorBehindMeters);
    const double end = std::min(
            roadCourse.totalLengthMeters,
            distanceMeters + FloorAheadMeters);
    const int nextBuffer = 1 - activeFloorBuffer;
    floorBuffers[std::size_t(nextBuffer)]->setCourseRange(
            roadCourse, start, end);
    if (!floorBuffers[std::size_t(nextBuffer)]->ready()) return;
    activeFloorBuffer = nextBuffer;
    emit floorGeometryChanged();
}

void WorkoutGame3DViewModel::rebuildTrees(double distanceMeters)
{
    if (!roadCourse.ready) return;
    const int bucket = int(std::floor(distanceMeters / TreeSpacingMeters));
    if (bucket == treeBucket) return;
    treeBucket = bucket;
    visibleTrees.clear();
    for (int offset = -3; offset <= 15; ++offset) {
        const int slot = bucket + offset;
        if (slot < 0) continue;
        const double distance =
                (double(slot) + 0.5) * TreeSpacingMeters;
        if (distance > roadCourse.totalLengthMeters) break;
        const WorkoutGameRoadSample sample = WorkoutGameRoadCourseBuilder::sample(
                roadCourse, distance);
        if (!sample.ready) continue;
        const std::uint32_t random = mix(
                roadCourse.seed ^ std::uint32_t(slot * 0x9e3779b9u));
        const double side = (random & 1u) == 0u ? -1.0 : 1.0;
        const double lateral = side * (3.3 + double((random >> 8) & 255u) / 85.0);
        const double rightX = std::cos(sample.center.headingRadians);
        const double rightZ = -std::sin(sample.center.headingRadians);
        QVariantMap tree;
        tree.insert(QStringLiteral("x"), sample.center.xMeters + lateral * rightX);
        tree.insert(QStringLiteral("y"), sample.baseElevationMeters - 0.10);
        tree.insert(QStringLiteral("z"), sample.center.zMeters + lateral * rightZ);
        tree.insert(QStringLiteral("scale"),
                    0.75 + double((random >> 16) & 255u) / 510.0);
        tree.insert(QStringLiteral("variant"), int((random >> 24) & 3u));
        visibleTrees.push_back(tree);
    }
    emit treesChanged();
}
