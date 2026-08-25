/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DViewModel.h"

#include "WorkoutGame3DFeatureAsset.h"
#include "WorkoutGameBermGeometry.h"
#include "WorkoutGame3DTerrainProfile.h"
#include "WorkoutGameFeatureChallenge.h"
#include "WorkoutGameFeatureGeometry.h"
#include "WorkoutGameRootGeometry.h"
#include "WorkoutGameTrailBranch.h"

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
constexpr double TreeCrownRadiusMeters = 1.35;
constexpr double CameraCorridorClearanceMeters = 0.85;

double finiteOrZero(double value)
{
    return std::isfinite(value) ? value : 0.0;
}

double horizontalDistanceToSegmentSquared(
        double pointX,
        double pointZ,
        double startX,
        double startZ,
        double endX,
        double endZ)
{
    const double segmentX = endX - startX;
    const double segmentZ = endZ - startZ;
    const double lengthSquared = segmentX * segmentX + segmentZ * segmentZ;
    const double projection = lengthSquared > 1.0e-9
            ? std::clamp(((pointX - startX) * segmentX
                    + (pointZ - startZ) * segmentZ) / lengthSquared,
                    0.0, 1.0)
            : 0.0;
    const double closestX = startX + projection * segmentX;
    const double closestZ = startZ + projection * segmentZ;
    const double offsetX = pointX - closestX;
    const double offsetZ = pointZ - closestZ;
    return offsetX * offsetX + offsetZ * offsetZ;
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
            WorkoutGame3DGeometry::Layer::Trail)),
    berm(std::make_unique<WorkoutGame3DGeometry>(
            WorkoutGame3DGeometry::Layer::Berm)),
    bypass(std::make_unique<WorkoutGame3DGeometry>(
            WorkoutGame3DGeometry::Layer::Bypass))
{
    for (std::unique_ptr<WorkoutGame3DGeometry> &buffer : floorBuffers) {
        buffer = std::make_unique<WorkoutGame3DGeometry>(
                WorkoutGame3DGeometry::Layer::ForestFloor);
    }
    for (std::unique_ptr<WorkoutGame3DGeometry> &buffer : rootBuffers) {
        buffer = std::make_unique<WorkoutGame3DGeometry>(
                WorkoutGame3DGeometry::Layer::Roots);
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
    rollerChallengePieceIndices.clear();
    for (std::size_t index = 0; index < roadCourse.pieces.size(); ++index) {
        const WorkoutGameRoadPiece &piece = roadCourse.pieces[index];
        if (piece.challenge.enabled
                && piece.terrain == WorkoutGameTerrainKind::Rollers) {
            rollerChallengePieceIndices.push_back(index);
        }
    }
    rebuildPowerProfile(course);
    trail->setCourse(roadCourse);
    berm->setCourse(roadCourse);
    bypass->setCourse(roadCourse);
    floorBucket = std::numeric_limits<int>::min();
    featureBucket = std::numeric_limits<int>::min();
    treeBucket = std::numeric_limits<int>::min();
    visibleTrees.clear();
    currentFeatureHud = {};
    currentFeatureName.clear();
    currentFeatureActionText.clear();
    currentFeatureStatus.clear();
    currentReadinessPercent = 0;
    currentWorkoutProgress = 0.0;
    currentGradePercent = 0.0;
    riderPumpMeters = 0.0;
    riderPoseInitialized = false;
    rootCompressionInitialized = false;
    lastRiderPoseTimeMs = -1;
    rebuildFloor(0.0);
    sceneReady = roadCourse.ready && trail->ready()
            && floorBuffers[std::size_t(activeFloorBuffer)]->ready();
    rebuildFeatures(0.0);
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
    cameraGroundY = sample.visualGroundElevationMeters();
    const double authoritativeAir = std::max(
            0.0, finiteOrZero(frame.world.rider.airHeightMeters()));
    double visualGround = sample.center.elevationMeters;
    if (frame.feature.route == WorkoutGameRoute::SafeBypass) {
        const WorkoutGame3DTerrainProfileSnapshot terrain =
                WorkoutGame3DTerrainProfile::build(
                    sample, distanceMeters, roadCourse.seed);
        double treadLift = WorkoutGameTrailBranch::treadLiftMeters(0.0);
        for (const WorkoutGameRoadPiece &piece : roadCourse.pieces) {
            if (piece.challenge.enabled
                    && distanceMeters
                        >= piece.challenge.bypassStartDistanceMeters
                    && distanceMeters
                        <= piece.challenge.bypassEndDistanceMeters) {
                const double length =
                        piece.challenge.bypassEndDistanceMeters
                        - piece.challenge.bypassStartDistanceMeters;
                if (length > 0.0) {
                    const double branchBlend =
                            WorkoutGameTrailBranch::blend(
                                (distanceMeters
                                 - piece.challenge
                                    .bypassStartDistanceMeters)
                                / length);
                    treadLift = WorkoutGameTrailBranch::treadLiftMeters(
                            branchBlend);
                }
                break;
            }
        }
        visualGround = terrain.ready
                ? WorkoutGame3DTerrainProfile::elevationAtLateral(
                    terrain, lateral) + treadLift
                : sample.visualGroundElevationMeters();
        if (sample.terrain == WorkoutGameTerrainKind::Roots
                && frame.feature.route == WorkoutGameRoute::SafeBypass) {
            visualGround -= sample.surfaceOffsetMeters;
        }
        if (sample.terrain == WorkoutGameTerrainKind::Berm
                && sample.pieceIndex < roadCourse.pieces.size()) {
            const WorkoutGameRoadPiece &piece =
                    roadCourse.pieces[sample.pieceIndex];
            const WorkoutGameBermGeometryProfile bermProfile =
                    WorkoutGameBermGeometry::profile(piece.difficulty);
            const double local = distanceMeters
                    - piece.challenge.obstacleDistanceMeters;
            visualGround = sample.visualGroundElevationMeters()
                    + bermProfile.surfaceOffsetMeters(
                        local, lateral,
                        bermProfile.halfWidthMeters(local),
                        piece.turnRadians);
        }
    }
    riderPositionY = visualGround + authoritativeAir;
    riderPositionZ = sample.center.zMeters + lateral * rightZ;
    riderHeadingDegrees = sample.center.headingRadians * 180.0 / Pi;
    updateCameraPose(distanceMeters, lateral);
    riderPitchDegrees = finiteOrZero(frame.world.rider.pitchDegrees);
    double targetRiderRollDegrees =
            finiteOrZero(frame.world.rider.rollDegrees);
    if (frame.world.terrain == WorkoutGameTerrainKind::Berm
            && sample.pieceIndex < roadCourse.pieces.size()) {
        const WorkoutGameRoadPiece &piece =
                roadCourse.pieces[sample.pieceIndex];
        const WorkoutGameBermGeometryProfile bermProfile =
                WorkoutGameBermGeometry::profile(piece.difficulty);
        targetRiderRollDegrees = bermProfile.riderWorldRollRadians(
                distanceMeters - piece.challenge.obstacleDistanceMeters,
                piece.turnRadians,
                std::max(0.0, frame.simulation.speedKph) / 3.6,
                frame.feature.route == WorkoutGameRoute::SafeBypass)
                * 180.0 / Pi;
    }
    constexpr double MaximumBermRollStepDegrees = 1.5;
    if (riderPoseInitialized
            && frame.world.terrain == WorkoutGameTerrainKind::Berm) {
        riderRollDegrees += std::clamp(
                targetRiderRollDegrees - riderRollDegrees,
                -MaximumBermRollStepDegrees,
                MaximumBermRollStepDegrees);
    } else {
        riderRollDegrees = targetRiderRollDegrees;
    }
    riderPoseInitialized = true;
    if (frame.world.terrain != WorkoutGameTerrainKind::Roots) {
        rootCompressionInitialized = false;
    }
    if (frame.world.terrain == WorkoutGameTerrainKind::Roots
            && frame.feature.route == WorkoutGameRoute::MainLine
            && !frame.world.rider.airborne) {
        const double compression = std::clamp(0.5 * (
                finiteOrZero(frame.world.rider.rearSuspension)
                + finiteOrZero(frame.world.rider.frontSuspension)), 0.0, 1.0);
        if (!rootCompressionInitialized) {
            previousRootCompression = compression;
            rootCompressionInitialized = true;
        }
        const double compressionDelta = compression - previousRootCompression;
        const double target = std::clamp(
                -0.10 * compressionDelta, -0.05, 0.025);
        const double elapsedSeconds = lastRiderPoseTimeMs >= 0
                ? std::clamp(double(frame.simulation.workoutTimeMs
                                    - lastRiderPoseTimeMs) / 1000.0,
                             0.0, 0.25)
                : 0.08;
        const double blend = 1.0 - std::exp(
                -elapsedSeconds / 0.08);
        riderPumpMeters += (target - riderPumpMeters) * blend;
        riderPumpMeters = std::clamp(riderPumpMeters, -0.05, 0.025);
        previousRootCompression = compression;
    } else if (frame.world.terrain == WorkoutGameTerrainKind::Rollers
            && frame.feature.route == WorkoutGameRoute::MainLine
            && !frame.world.rider.airborne) {
        const double compression = std::clamp(0.5 * (
                finiteOrZero(frame.world.rider.rearSuspension)
                + finiteOrZero(frame.world.rider.frontSuspension)), 0.0, 1.0);
        double profilePose = 0.0;
        for (const std::size_t index : rollerChallengePieceIndices) {
            const WorkoutGameRoadPiece &piece = roadCourse.pieces[index];
            const WorkoutGameFeatureGeometryProfile profile =
                    WorkoutGameFeatureGeometry::profile(
                        piece.terrain, piece.difficulty);
            const double local = distanceMeters
                    - piece.challenge.obstacleDistanceMeters;
            if (!profile.ready || local < profile.plateauStartMeters
                    || local > profile.plateauEndMeters) {
                continue;
            }
            const double crestPhase = profile.heightMeters > 0.0
                    ? std::clamp(profile.surfaceOffset(local)
                            / profile.heightMeters, 0.0, 1.0)
                    : 0.0;
            profilePose = 0.06 - 0.16 * crestPhase;
            break;
        }
        const double suspensionFineMotion =
                (0.5 - compression) * 0.04;
        riderPumpMeters = std::clamp(
                profilePose + suspensionFineMotion, -0.10, 0.06);
    } else {
        riderPumpMeters = 0.0;
    }
    lastRiderPoseTimeMs = frame.simulation.workoutTimeMs;
    currentPedalAngle = std::fmod(
            finiteOrZero(frame.riderPedalCycles) * 360.0, 360.0);
    currentSpeedKph = std::max(0.0, finiteOrZero(frame.simulation.speedKph));
    currentDistanceMeters = distanceMeters;
    currentWorkoutTimeSeconds = int(std::clamp<std::int64_t>(
            frame.simulation.workoutTimeMs / 1000, 0, 24 * 60 * 60));
    currentWorkoutProgress = courseDurationMs > 0
            ? std::clamp(double(frame.simulation.workoutTimeMs)
                    / double(courseDurationMs), 0.0, 1.0)
            : 0.0;
    currentGradePercent = std::clamp(
            finiteOrZero(frame.world.gradePercent), -30.0, 30.0);
    currentTerrainName = terrainText(frame.world.terrain);
    currentFeatureStatus = featureText(frame.feature);
    currentReadinessPercent = int(std::lround(std::clamp(
            finiteOrZero(frame.feature.readiness), 0.0, 1.0) * 100.0));
    currentFeatureHud = WorkoutGameFeatureHud::build(
            frame.feature, frame.simulation, currentTargetWatts);
    currentFeatureName = currentFeatureHud.visible
            ? terrainText(currentFeatureHud.terrain) : QString();
    currentFeatureActionText = featureActionText(currentFeatureHud);
    rebuildFloor(currentDistanceMeters);
    rebuildFeatures(currentDistanceMeters);
    rebuildTrees(currentDistanceMeters);
    emit sceneChanged();
}

void WorkoutGame3DViewModel::rebuildPowerProfile(
        const WorkoutGameCourse &course)
{
    currentPowerProfile.clear();
    currentPowerProfileMaximumWatts = 1.0;
    courseDurationMs = course.status == WorkoutGameCourseStatus::Ready
            ? std::max<std::int64_t>(0, course.durationMs) : 0;
    if (courseDurationMs <= 0) return;

    for (const WorkoutGameSection &section : course.sections) {
        const double targetWatts = std::max(
                0.0, finiteOrZero(section.targetWatts));
        const WorkoutGameFeatureChallengeProfile challenge =
                WorkoutGameFeatureChallenge::profile(section);
        const double requiredWatts = challenge.enabled
                && challenge.minimumEffortRatio > 0.0
                ? targetWatts * challenge.minimumEffortRatio : 0.0;
        currentPowerProfileMaximumWatts = std::max(
                currentPowerProfileMaximumWatts,
                std::max(targetWatts, requiredWatts));
    }
    currentPowerProfileMaximumWatts *= 1.1;

    for (const WorkoutGameSection &section : course.sections) {
        const double start = std::clamp(
                double(section.startMs) / double(courseDurationMs), 0.0, 1.0);
        const double end = std::clamp(
                double(section.startMs + section.durationMs)
                    / double(courseDurationMs),
                start, 1.0);
        const double targetWatts = std::max(
                0.0, finiteOrZero(section.targetWatts));
        const WorkoutGameFeatureChallengeProfile challenge =
                WorkoutGameFeatureChallenge::profile(section);
        QVariantMap values;
        values.insert(QStringLiteral("start"), start);
        values.insert(QStringLiteral("end"), end);
        values.insert(QStringLiteral("targetWatts"), targetWatts);
        values.insert(QStringLiteral("height"),
                      targetWatts / currentPowerProfileMaximumWatts);
        values.insert(QStringLiteral("challenge"), challenge.enabled);
        if (challenge.enabled) {
            const double span = end - start;
            values.insert(QStringLiteral("challengeStart"),
                          start + span * challenge.measurementStartProgress);
            values.insert(QStringLiteral("challengeEnd"),
                          start + span * challenge.decisionProgress);
        }
        currentPowerProfile.append(values);
    }
}

void WorkoutGame3DViewModel::updateCameraPose(
        double distanceMeters,
        double lateralMeters)
{
    const double cameraDistance = std::max(
            0.0, distanceMeters - cameraBackDistanceMeters);
    const WorkoutGameRoadSample cameraSample =
            WorkoutGameRoadCourseBuilder::sample(roadCourse, cameraDistance);
    const double targetDistance = std::min(
            roadCourse.totalLengthMeters,
            distanceMeters + cameraLookAheadDistanceMeters);
    const WorkoutGameRoadSample targetSample =
            WorkoutGameRoadCourseBuilder::sample(roadCourse, targetDistance);
    if (!cameraSample.ready || !targetSample.ready) return;

    const double cameraForwardX = std::sin(
            cameraSample.center.headingRadians);
    const double cameraForwardZ = std::cos(
            cameraSample.center.headingRadians);
    const double cameraRightX = std::cos(
            cameraSample.center.headingRadians);
    const double cameraRightZ = -std::sin(
            cameraSample.center.headingRadians);
    const double missingBehind = std::max(
            0.0, cameraBackDistanceMeters - distanceMeters);
    cameraPositionX = cameraSample.center.xMeters
            - cameraForwardX * missingBehind
            - cameraRightX * cameraSideDistanceMeters;
    cameraPositionZ = cameraSample.center.zMeters
            - cameraForwardZ * missingBehind
            - cameraRightZ * cameraSideDistanceMeters;
    cameraPositionY = cameraSample.visualGroundElevationMeters()
            + cameraHeightDistanceMeters;

    const double targetForwardX = std::sin(
            targetSample.center.headingRadians);
    const double targetForwardZ = std::cos(
            targetSample.center.headingRadians);
    const double missingAhead = std::max(
            0.0, distanceMeters + cameraLookAheadDistanceMeters
                    - roadCourse.totalLengthMeters);
    cameraTargetPositionX = targetSample.center.xMeters
            + targetForwardX * missingAhead;
    cameraTargetPositionZ = targetSample.center.zMeters
            + targetForwardZ * missingAhead;
    cameraTargetPositionY = targetSample.visualGroundElevationMeters()
            + cameraTargetHeightDistanceMeters;

    const WorkoutGameRoadSample riderSample =
            WorkoutGameRoadCourseBuilder::sample(roadCourse, distanceMeters);
    if (!riderSample.ready
            || riderSample.terrain != WorkoutGameTerrainKind::Berm
            || riderSample.pieceIndex >= roadCourse.pieces.size()) {
        return;
    }
    const WorkoutGameRoadPiece &piece =
            roadCourse.pieces[riderSample.pieceIndex];
    if (!piece.challenge.enabled) return;
    const WorkoutGameBermGeometryProfile berm =
            WorkoutGameBermGeometry::profile(piece.difficulty);
    const double local = distanceMeters
            - piece.challenge.obstacleDistanceMeters;
    if (local <= berm.startMeters || local >= berm.endMeters) return;
    const double progress = std::clamp(
            (local - berm.startMeters) / (berm.endMeters - berm.startMeters),
            0.0, 1.0);
    const double blend = std::pow(std::sin(Pi * progress), 2.0);
    constexpr double BermCameraBackMeters = 6.2;
    constexpr double BermCameraLookAheadMeters = 2.5;
    const double riderForwardX = std::sin(
            riderSample.center.headingRadians);
    const double riderForwardZ = std::cos(
            riderSample.center.headingRadians);
    const double riderRightX = std::cos(
            riderSample.center.headingRadians);
    const double riderRightZ = -std::sin(
            riderSample.center.headingRadians);
    const double chaseX = riderSample.center.xMeters
            + lateralMeters * riderRightX
            - BermCameraBackMeters * riderForwardX;
    const double chaseZ = riderSample.center.zMeters
            + lateralMeters * riderRightZ
            - BermCameraBackMeters * riderForwardZ;
    const double chaseY = riderSample.visualGroundElevationMeters()
            + cameraHeightDistanceMeters;
    const WorkoutGameRoadSample bermTarget =
            WorkoutGameRoadCourseBuilder::sample(
                roadCourse,
                std::min(roadCourse.totalLengthMeters,
                    distanceMeters + BermCameraLookAheadMeters));
    if (!bermTarget.ready) return;
    const double targetRightX = std::cos(
            bermTarget.center.headingRadians);
    const double targetRightZ = -std::sin(
            bermTarget.center.headingRadians);
    const double chaseTargetX = bermTarget.center.xMeters
            + lateralMeters * targetRightX;
    const double chaseTargetZ = bermTarget.center.zMeters
            + lateralMeters * targetRightZ;
    const double chaseTargetY = bermTarget.visualGroundElevationMeters()
            + cameraTargetHeightDistanceMeters;
    cameraPositionX += (chaseX - cameraPositionX) * blend;
    cameraPositionY += (chaseY - cameraPositionY) * blend;
    cameraPositionZ += (chaseZ - cameraPositionZ) * blend;
    cameraTargetPositionX += (chaseTargetX - cameraTargetPositionX) * blend;
    cameraTargetPositionY += (chaseTargetY - cameraTargetPositionY) * blend;
    cameraTargetPositionZ += (chaseTargetZ - cameraTargetPositionZ) * blend;
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

QString WorkoutGame3DViewModel::featureActionText(
        const WorkoutGameFeatureHudSnapshot &hud)
{
    switch (hud.state) {
    case WorkoutGameFeatureHudState::Prepare:
        return tr("Prepare");
    case WorkoutGameFeatureHudState::Measure:
        return tr("Build power");
    case WorkoutGameFeatureHudState::Committed:
        return hud.route == WorkoutGameRoute::SafeBypass
                ? tr("Safe line") : tr("Line committed");
    case WorkoutGameFeatureHudState::ActNow:
        return tr("Ride now");
    case WorkoutGameFeatureHudState::Complete:
        return tr("Complete");
    case WorkoutGameFeatureHudState::Bypass:
        return tr("Safe line");
    case WorkoutGameFeatureHudState::Hidden:
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
        feature.insert(QStringLiteral("y"),
                       sample.visualGroundElevationMeters());
        feature.insert(QStringLiteral("z"), sample.center.zMeters);
        feature.insert(QStringLiteral("yaw"),
                       sample.center.headingRadians * 180.0 / Pi);
        feature.insert(QStringLiteral("difficulty"), piece.difficulty);
        const WorkoutGame3DFeatureAssetSnapshot asset =
                WorkoutGame3DFeatureAsset::place(roadCourse, piece);
        if (asset.ready) {
            feature.insert(QStringLiteral("assetX"), asset.xMeters);
            feature.insert(QStringLiteral("assetY"), asset.yMeters);
            feature.insert(QStringLiteral("assetZ"), asset.zMeters);
            feature.insert(QStringLiteral("assetYaw"), asset.yawDegrees);
            feature.insert(QStringLiteral("assetPitch"), asset.pitchDegrees);
            feature.insert(QStringLiteral("assetScaleY"), asset.scaleY);
            feature.insert(QStringLiteral("assetScaleZ"), asset.scaleZ);
        }
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
        rootBuffers[0]->setCourse(roadCourse);
        rootBuffers[1]->setCourse(roadCourse);
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
    rootBuffers[std::size_t(nextBuffer)]->setCourseRange(
            roadCourse, start, end);
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
        const double scale =
                0.75 + double((random >> 16) & 255u) / 510.0;
        const double crownRadius = TreeCrownRadiusMeters * scale;
        const double treeX = sample.center.xMeters + lateral * rightX;
        const double treeZ = sample.center.zMeters + lateral * rightZ;
        const WorkoutGame3DTerrainProfileSnapshot terrain =
                WorkoutGame3DTerrainProfile::build(
                    sample, distance, roadCourse.seed);
        if (!terrain.ready) continue;
        const double treeY = WorkoutGame3DTerrainProfile::elevationAtLateral(
                terrain, lateral);
        const double requiredClearance =
                crownRadius + CameraCorridorClearanceMeters;
        if (horizontalDistanceToSegmentSquared(
                    treeX, treeZ,
                    cameraPositionX, cameraPositionZ,
                    cameraTargetPositionX, cameraTargetPositionZ)
                < requiredClearance * requiredClearance) {
            continue;
        }
        QVariantMap tree;
        tree.insert(QStringLiteral("x"), treeX);
        tree.insert(QStringLiteral("y"), treeY);
        tree.insert(QStringLiteral("z"), treeZ);
        tree.insert(QStringLiteral("distance"), distance);
        tree.insert(QStringLiteral("lateral"), lateral);
        tree.insert(QStringLiteral("scale"), scale);
        tree.insert(QStringLiteral("crownRadius"), crownRadius);
        tree.insert(QStringLiteral("variant"), int((random >> 24) & 3u));
        visibleTrees.push_back(tree);
    }
    emit treesChanged();
}
