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
#include "WorkoutGameClimbGeometry.h"
#include "WorkoutGame3DTerrainProfile.h"
#include "WorkoutGameFeatureChallenge.h"
#include "WorkoutGameFeatureGeometry.h"
#include "WorkoutGameRockGardenGeometry.h"
#include "WorkoutGameRockSlabGeometry.h"
#include "WorkoutGameRiderAnimation.h"
#include "WorkoutGameRootGeometry.h"
#include "WorkoutGameSkinnyGeometry.h"
#include "WorkoutGameTabletopGeometry.h"
#include "WorkoutGameTrailBranch.h"

#include <QByteArray>
#include <QMetaObject>
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
constexpr double TreeSpacingMeters = 7.0;
constexpr int MaximumVisibleTrees = 18;
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
    bypass(std::make_unique<WorkoutGame3DGeometry>(
            WorkoutGame3DGeometry::Layer::Bypass)),
    gapJump(std::make_unique<WorkoutGame3DGeometry>(
            WorkoutGame3DGeometry::Layer::GapJump))
{
    for (std::unique_ptr<WorkoutGame3DGeometry> &buffer : floorBuffers) {
        buffer = std::make_unique<WorkoutGame3DGeometry>(
                WorkoutGame3DGeometry::Layer::ForestFloor);
    }
    for (std::unique_ptr<WorkoutGame3DGeometry> &buffer : bermBuffers) {
        buffer = std::make_unique<WorkoutGame3DGeometry>(
                WorkoutGame3DGeometry::Layer::Berm);
    }
    for (std::unique_ptr<WorkoutGame3DGeometry> &buffer
            : forestDressingBuffers) {
        buffer = std::make_unique<WorkoutGame3DGeometry>(
                WorkoutGame3DGeometry::Layer::ForestDressing);
    }
    for (std::unique_ptr<WorkoutGame3DGeometry> &buffer : climbBuffers) {
        buffer = std::make_unique<WorkoutGame3DGeometry>(
                WorkoutGame3DGeometry::Layer::Climb);
    }
    for (std::unique_ptr<WorkoutGame3DGeometry> &buffer : rootBuffers) {
        buffer = std::make_unique<WorkoutGame3DGeometry>(
                WorkoutGame3DGeometry::Layer::Roots);
    }
    for (std::unique_ptr<WorkoutGame3DGeometry> &buffer
            : rockGardenBuffers) {
        buffer = std::make_unique<WorkoutGame3DGeometry>(
                WorkoutGame3DGeometry::Layer::RockGarden);
    }
    for (std::unique_ptr<WorkoutGame3DGeometry> &buffer
            : rockSlabBuffers) {
        buffer = std::make_unique<WorkoutGame3DGeometry>(
                WorkoutGame3DGeometry::Layer::RockSlab);
    }
    for (std::unique_ptr<WorkoutGame3DGeometry> &buffer : skinnyBuffers) {
        buffer = std::make_unique<WorkoutGame3DGeometry>(
                WorkoutGame3DGeometry::Layer::Skinny);
    }
    chunkBuilder.setCompletionCallback(
            [this]() { scheduleReadyFloorChunk(); });
}

WorkoutGame3DViewModel::~WorkoutGame3DViewModel()
{
    chunkBuilder.setCompletionCallback({});
    chunkBuilder.shutdown();
}

void WorkoutGame3DViewModel::setCourse(
        const WorkoutGameCourse &course,
        double ftpWatts)
{
    ++courseGeneration;
    roadCourse = WorkoutGameRoadCourseBuilder::build(course, ftpWatts);
    roadCourseSnapshot =
            std::make_shared<const WorkoutGameRoadCourse>(roadCourse);
    rollerChallengePieceIndices.clear();
    climbChallengePieceIndices.clear();
    for (std::size_t index = 0; index < roadCourse.pieces.size(); ++index) {
        const WorkoutGameRoadPiece &piece = roadCourse.pieces[index];
        if (piece.challenge.enabled
                && piece.terrain == WorkoutGameTerrainKind::Rollers) {
            rollerChallengePieceIndices.push_back(index);
        } else if (piece.challenge.enabled
                && piece.terrain == WorkoutGameTerrainKind::Climb) {
            climbChallengePieceIndices.push_back(index);
        }
    }
    rebuildPowerProfile(course);
    trail->setCourse(roadCourse);
    bypass->setCourse(roadCourse);
    gapJump->setCourse(roadCourse);
    floorBucket = std::numeric_limits<int>::min();
    requestedFloorBucket = std::numeric_limits<int>::min();
    featureBucket = std::numeric_limits<int>::min();
    treeBucket = std::numeric_limits<int>::min();
    treePresentationBucket = std::numeric_limits<int>::min();
    visibleTrees.clear();
    currentFeatureHud = {};
    currentFeatureName.clear();
    currentFeatureActionText.clear();
    currentFeatureStatus.clear();
    currentReadinessPercent = 0;
    currentWorkoutProgress = 0.0;
    currentGradePercent = 0.0;
    riderPumpMeters = 0.0;
    currentRiderAirHeightMeters = 0.0;
    currentLandingImpact = 0.0;
    previousLandingImpact = 0.0;
    currentLandingEffectStrength = 0.0;
    lastSuccessActionId = 0;
    currentSuccessEffectText.clear();
    currentRiderStandingBlend = 0.0;
    currentRiderPedalEffort = 0.0;
    currentRearSuspensionCompression = 0.0;
    currentFrontSuspensionCompression = 0.0;
    currentRiderWalking = false;
    currentRiderPoseState = QStringLiteral("pedal");
    riderPoseInitialized = false;
    rootCompressionInitialized = false;
    rockCompressionInitialized = false;
    slabCompressionInitialized = false;
    lastRiderPoseTimeMs = -1;
    cameraPresentationController.reset();
    cameraPresentationSnapshot = {};
    cameraPoseInitialized = false;
    lastCameraPoseTimeMs = 0;
    rebuildFloor(0.0, true);
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
    installReadyFloorChunk();
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
    const bool completedAirFeature = frame.feature.ready
            && frame.feature.outcome == WorkoutGameFeatureOutcome::Completed
            && (frame.feature.motion == WorkoutGameFeatureMotion::Jump
                || frame.feature.motion == WorkoutGameFeatureMotion::Drop);
    const double authoritativeAir = std::max(0.0, finiteOrZero(
            completedAirFeature
                ? frame.world.visualAirHeightMeters()
                : frame.world.rider.airHeightMeters()));
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
                ? WorkoutGame3DTerrainProfile::bypassSurfaceElevationMeters(
                    sample, distanceMeters, roadCourse.seed,
                    lateral, treadLift)
                : sample.visualGroundElevationMeters();
        if ((sample.terrain == WorkoutGameTerrainKind::Roots
                || sample.terrain == WorkoutGameTerrainKind::RockSlab)
                && frame.feature.route == WorkoutGameRoute::SafeBypass) {
            visualGround -= sample.surfaceOffsetMeters;
        }
        if (sample.terrain == WorkoutGameTerrainKind::RockGarden
                && frame.feature.route == WorkoutGameRoute::SafeBypass
                && sample.pieceIndex < roadCourse.pieces.size()) {
            const WorkoutGameRoadPiece &piece =
                    roadCourse.pieces[sample.pieceIndex];
            const WorkoutGameRockGardenGeometryProfile rocks =
                    WorkoutGameRockGardenGeometry::profile(
                        piece.difficulty);
            const double local = distanceMeters
                    - piece.challenge.obstacleDistanceMeters;
            visualGround += rocks.surfaceOffsetMeters(local, lateral)
                    - sample.surfaceOffsetMeters;
        }
    }
    if (sample.terrain == WorkoutGameTerrainKind::Berm
            && sample.pieceIndex < roadCourse.pieces.size()) {
        const WorkoutGameRoadPiece &piece =
                roadCourse.pieces[sample.pieceIndex];
        const WorkoutGameBermGeometryProfile bermProfile =
                WorkoutGameBermGeometry::profile(piece.difficulty);
        const double local = distanceMeters
                - piece.geometryAnchorDistanceMeters;
        visualGround = sample.visualGroundElevationMeters()
                + bermProfile.surfaceOffsetMeters(
                    local, lateral,
                    bermProfile.halfWidthMeters(local),
                    piece.turnRadians);
    }
    cameraGroundY = visualGround;
    riderPositionY = visualGround + authoritativeAir;
    riderPositionZ = sample.center.zMeters + lateral * rightZ;
    riderHeadingDegrees = sample.center.headingRadians * 180.0 / Pi;
    const bool featureCritical = frame.feature.ready
            && frame.feature.phase != WorkoutGameFeaturePhase::None
            && frame.feature.phase != WorkoutGameFeaturePhase::Recovery;
    cameraPresentationSnapshot = cameraPresentationController.update({
        frame.simulation.workoutTimeMs,
        currentWatts,
        double(currentCadenceRpm),
        featureCritical,
        frame.world.rider.airborne
    });
    updateCameraPose(
            distanceMeters, lateral, frame.simulation.workoutTimeMs);
    riderPitchDegrees = std::clamp(
            finiteOrZero(frame.world.rider.pitchDegrees), -35.0, 35.0);
    double targetRiderRollDegrees =
            finiteOrZero(frame.world.rider.rollDegrees);
    if (frame.world.terrain == WorkoutGameTerrainKind::Berm
            && sample.pieceIndex < roadCourse.pieces.size()) {
        const WorkoutGameRoadPiece &piece =
                roadCourse.pieces[sample.pieceIndex];
        const WorkoutGameBermGeometryProfile bermProfile =
                WorkoutGameBermGeometry::profile(piece.difficulty);
        targetRiderRollDegrees = bermProfile.riderWorldRollRadians(
                distanceMeters - piece.geometryAnchorDistanceMeters,
                piece.turnRadians,
                std::max(0.0, frame.simulation.speedKph) / 3.6,
                frame.feature.bermLineBias)
                * 180.0 / Pi;
    } else if (frame.world.terrain == WorkoutGameTerrainKind::Skinny
            && frame.feature.route == WorkoutGameRoute::MainLine
            && sample.pieceIndex < roadCourse.pieces.size()) {
        const WorkoutGameRoadPiece &piece =
                roadCourse.pieces[sample.pieceIndex];
        const WorkoutGameSkinnyGeometryProfile skinny =
                WorkoutGameSkinnyGeometry::profile(piece.difficulty);
        targetRiderRollDegrees = skinny.balanceRollDegrees(
                distanceMeters - piece.challenge.obstacleDistanceMeters);
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
    currentRiderAirHeightMeters = authoritativeAir;
    currentLandingImpact = std::clamp(
            finiteOrZero(frame.world.landingImpact), 0.0, 1.0);
    constexpr double LandingEffectThreshold = 0.08;
    if (currentLandingImpact > LandingEffectThreshold
            && previousLandingImpact <= LandingEffectThreshold) {
        currentLandingEffectStrength = currentLandingImpact;
        ++currentLandingEffectId;
    }
    previousLandingImpact = currentLandingImpact;
    if (frame.feature.ready
            && frame.feature.phase == WorkoutGameFeaturePhase::Recovery
            && frame.feature.outcome == WorkoutGameFeatureOutcome::Completed
            && frame.feature.actionId != 0
            && frame.feature.actionId != lastSuccessActionId) {
        lastSuccessActionId = frame.feature.actionId;
        currentSuccessEffectText = tr("%1 clean").arg(
                terrainText(frame.feature.terrain));
        ++currentSuccessEffectId;
    }
    currentRiderWalking = frame.world.rider.walking;
    const WorkoutGameRiderAnimationTarget animationTarget =
            WorkoutGameRiderAnimation::target({
                currentWatts,
                currentTargetWatts,
                finiteOrZero(frame.world.gradePercent),
                double(currentCadenceRpm),
                currentRiderWalking,
                frame.world.rider.airborne
            });
    double targetStandingBlend = 0.0;
    if (frame.world.terrain == WorkoutGameTerrainKind::Climb) {
        const auto climbPiece = std::find_if(
                climbChallengePieceIndices.begin(),
                climbChallengePieceIndices.end(),
                [this, &sample](std::size_t index) {
                    return roadCourse.pieces[index].sourceSectionIndex
                            == roadCourse.pieces[sample.pieceIndex]
                                .sourceSectionIndex;
                });
        if (climbPiece != climbChallengePieceIndices.end()) {
            const WorkoutGameRoadPiece &piece = roadCourse.pieces[*climbPiece];
            const WorkoutGameClimbGeometryProfile climb =
                    WorkoutGameClimbGeometry::profile(piece.difficulty);
            const double effortRatio = currentTargetWatts > 0.0
                    ? currentWatts / currentTargetWatts : 0.0;
            targetStandingBlend = climb.standingBlend(
                    effortRatio, double(currentCadenceRpm),
                    currentRiderWalking)
                    * climb.crestRelease(
                        distanceMeters
                            - piece.challenge.obstacleDistanceMeters);
        }
    }
    targetStandingBlend = std::max(
            targetStandingBlend, animationTarget.standingBlend);
    const double poseElapsedSeconds = lastRiderPoseTimeMs >= 0
            ? std::clamp(double(frame.simulation.workoutTimeMs
                                - lastRiderPoseTimeMs) / 1000.0,
                         0.0, 0.25)
            : 0.08;
    const double standingTimeConstant = targetStandingBlend
                > currentRiderStandingBlend ? 0.22 : 0.30;
    const double standingBlend = 1.0 - std::exp(
            -poseElapsedSeconds / standingTimeConstant);
    currentRiderStandingBlend += (targetStandingBlend
            - currentRiderStandingBlend) * standingBlend;
    currentRiderStandingBlend = std::clamp(
            currentRiderStandingBlend, 0.0, 1.0);
    const double effortBlend = 1.0 - std::exp(
            -poseElapsedSeconds / 0.16);
    currentRiderPedalEffort += (animationTarget.pedalEffortBlend
            - currentRiderPedalEffort) * effortBlend;
    currentRiderPedalEffort = std::clamp(
            currentRiderPedalEffort, 0.0, 1.0);
    const double suspensionBlend = 1.0 - std::exp(
            -poseElapsedSeconds / 0.055);
    currentRearSuspensionCompression += (std::clamp(
            finiteOrZero(frame.world.rider.rearSuspension), 0.0, 1.0)
            - currentRearSuspensionCompression) * suspensionBlend;
    currentFrontSuspensionCompression += (std::clamp(
            finiteOrZero(frame.world.rider.frontSuspension), 0.0, 1.0)
            - currentFrontSuspensionCompression) * suspensionBlend;
    if (frame.world.terrain != WorkoutGameTerrainKind::Roots) {
        rootCompressionInitialized = false;
    }
    if (frame.world.terrain != WorkoutGameTerrainKind::RockGarden) {
        rockCompressionInitialized = false;
    }
    if (frame.world.terrain != WorkoutGameTerrainKind::RockSlab) {
        slabCompressionInitialized = false;
    }
    if (frame.world.terrain == WorkoutGameTerrainKind::Tabletop
            && frame.feature.route == WorkoutGameRoute::MainLine) {
        double target = 0.0;
        if (frame.world.rider.airborne) {
            target = 0.045;
        } else if (currentLandingImpact > 0.01) {
            target = -0.10 * currentLandingImpact;
        } else if (sample.pieceIndex < roadCourse.pieces.size()) {
            const WorkoutGameRoadPiece &piece =
                    roadCourse.pieces[sample.pieceIndex];
            if (piece.challenge.enabled) {
                const WorkoutGameTabletopGeometryProfile tabletop =
                        WorkoutGameTabletopGeometry::profile(
                            piece.difficulty);
                const double local = distanceMeters
                        - piece.challenge.obstacleDistanceMeters;
                const double preloadStart = tabletop.lipMeters - 0.9;
                if (local >= preloadStart
                        && local <= tabletop.lipMeters) {
                    const double progress = std::clamp(
                            (local - preloadStart) / 0.9, 0.0, 1.0);
                    target = -0.075 * std::sin(Pi * progress);
                }
            }
        }
        const double elapsedSeconds = lastRiderPoseTimeMs >= 0
                ? std::clamp(double(frame.simulation.workoutTimeMs
                                    - lastRiderPoseTimeMs) / 1000.0,
                             0.0, 0.25)
                : 0.08;
        const double blend = 1.0 - std::exp(-elapsedSeconds / 0.09);
        riderPumpMeters += (target - riderPumpMeters) * blend;
        riderPumpMeters = std::clamp(riderPumpMeters, -0.10, 0.05);
    } else if (frame.world.terrain == WorkoutGameTerrainKind::Roots
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
    } else if (frame.world.terrain == WorkoutGameTerrainKind::RockGarden
            && frame.feature.route == WorkoutGameRoute::MainLine
            && !frame.world.rider.airborne) {
        const double compression = std::clamp(0.5 * (
                finiteOrZero(frame.world.rider.rearSuspension)
                + finiteOrZero(frame.world.rider.frontSuspension)), 0.0, 1.0);
        if (!rockCompressionInitialized) {
            previousRockCompression = compression;
            rockCompressionInitialized = true;
        }
        const double compressionDelta = compression
                - previousRockCompression;
        const double target = std::clamp(
                -0.16 * compressionDelta, -0.08, 0.04);
        const double elapsedSeconds = lastRiderPoseTimeMs >= 0
                ? std::clamp(double(frame.simulation.workoutTimeMs
                                    - lastRiderPoseTimeMs) / 1000.0,
                             0.0, 0.25)
                : 0.07;
        const double blend = 1.0 - std::exp(
                -elapsedSeconds / 0.07);
        riderPumpMeters += (target - riderPumpMeters) * blend;
        riderPumpMeters = std::clamp(riderPumpMeters, -0.08, 0.04);
        previousRockCompression = compression;
    } else if (frame.world.terrain == WorkoutGameTerrainKind::RockSlab
            && frame.feature.route == WorkoutGameRoute::MainLine
            && !frame.world.rider.airborne) {
        const double compression = std::clamp(0.5 * (
                finiteOrZero(frame.world.rider.rearSuspension)
                + finiteOrZero(frame.world.rider.frontSuspension)), 0.0, 1.0);
        if (!slabCompressionInitialized) {
            previousSlabCompression = compression;
            slabCompressionInitialized = true;
        }
        const double target = std::clamp(
                -0.14 * (compression - previousSlabCompression),
                -0.07, 0.035);
        const double elapsedSeconds = lastRiderPoseTimeMs >= 0
                ? std::clamp(double(frame.simulation.workoutTimeMs
                                    - lastRiderPoseTimeMs) / 1000.0,
                             0.0, 0.25)
                : 0.075;
        const double blend = 1.0 - std::exp(
                -elapsedSeconds / 0.075);
        riderPumpMeters += (target - riderPumpMeters) * blend;
        riderPumpMeters = std::clamp(riderPumpMeters, -0.07, 0.035);
        previousSlabCompression = compression;
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
    if (currentRiderWalking
            || frame.feature.route == WorkoutGameRoute::SafeBypass) {
        currentRiderPoseState = QStringLiteral("bypass");
    } else if (currentRiderAirHeightMeters > 0.05) {
        currentRiderPoseState = QStringLiteral("air");
    } else if (currentLandingImpact > 0.20) {
        currentRiderPoseState = QStringLiteral("land");
    } else if (frame.feature.ready
            && frame.feature.phase == WorkoutGameFeaturePhase::Action
            && (frame.feature.motion == WorkoutGameFeatureMotion::Jump
                || frame.feature.motion == WorkoutGameFeatureMotion::Drop)) {
        currentRiderPoseState = QStringLiteral("preload");
    } else if (frame.feature.ready
            && frame.feature.phase == WorkoutGameFeaturePhase::Action
            && frame.feature.motion == WorkoutGameFeatureMotion::Absorb) {
        currentRiderPoseState = QStringLiteral("absorb");
    } else if (std::abs(riderRollDegrees) > 2.0) {
        currentRiderPoseState = QStringLiteral("lean");
    } else if (currentCadenceRpm <= 5 && currentSpeedKph > 1.0) {
        currentRiderPoseState = QStringLiteral("coast");
    } else {
        currentRiderPoseState = QStringLiteral("pedal");
    }
    currentFeatureName = currentFeatureHud.visible
            ? terrainText(currentFeatureHud.terrain) : QString();
    if (currentFeatureHud.visible
            && currentFeatureHud.terrain == WorkoutGameTerrainKind::GapJump) {
        const WorkoutGameGapJumpLine line = frame.feature.gapLineLocked
                ? frame.feature.lockedGapLine
                : frame.feature.provisionalGapLine;
        currentFeatureName = gapJumpFeatureText(
                line,
                frame.feature.predictedApproachSpeedMetersPerSecond,
                frame.feature.gapLineLocked);
    }
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
        double lateralMeters,
        std::int64_t workoutTimeMs)
{
    const double previousX = cameraPositionX;
    const double previousY = cameraPositionY;
    const double previousZ = cameraPositionZ;
    const double previousTargetX = cameraTargetPositionX;
    const double previousTargetY = cameraTargetPositionY;
    const double previousTargetZ = cameraTargetPositionZ;
    const auto constrainCameraTravel = [this, previousX, previousY, previousZ,
                                        previousTargetX, previousTargetY,
                                        previousTargetZ, workoutTimeMs]() {
        const std::int64_t now = std::max<std::int64_t>(0, workoutTimeMs);
        if (!cameraPoseInitialized || now < lastCameraPoseTimeMs) {
            cameraPoseInitialized = true;
            lastCameraPoseTimeMs = now;
            return;
        }
        if (now == lastCameraPoseTimeMs) {
            cameraPositionX = previousX;
            cameraPositionY = previousY;
            cameraPositionZ = previousZ;
            cameraTargetPositionX = previousTargetX;
            cameraTargetPositionY = previousTargetY;
            cameraTargetPositionZ = previousTargetZ;
            return;
        }
        const double elapsedSeconds = std::clamp(
                double(now - lastCameraPoseTimeMs) / 1000.0,
                0.0, 0.10);
        constexpr double MaximumCameraSpeedMetersPerSecond = 17.0;
        const double maximumStep =
                MaximumCameraSpeedMetersPerSecond * elapsedSeconds;
        const double deltaX = cameraPositionX - previousX;
        const double deltaZ = cameraPositionZ - previousZ;
        const double distance = std::hypot(deltaX, deltaZ);
        if (distance > maximumStep && distance > 1.0e-9) {
            const double scale = maximumStep / distance;
            const double constrainedX = previousX + deltaX * scale;
            const double constrainedZ = previousZ + deltaZ * scale;
            cameraTargetPositionX += constrainedX - cameraPositionX;
            cameraTargetPositionZ += constrainedZ - cameraPositionZ;
            cameraPositionX = constrainedX;
            cameraPositionZ = constrainedZ;
        }
        lastCameraPoseTimeMs = now;
    };
    const double cameraDistance = std::max(
            0.0, distanceMeters - cameraBackDistanceMeters);
    const WorkoutGameRoadSample cameraSample =
            WorkoutGameRoadCourseBuilder::sampleVisual(
                roadCourse, cameraDistance);
    const double targetDistance = std::min(
            std::max(roadCourse.totalLengthMeters,
                     roadCourse.visualLengthMeters),
            distanceMeters + cameraLookAheadDistanceMeters);
    const WorkoutGameRoadSample targetSample =
            WorkoutGameRoadCourseBuilder::sampleVisual(
                roadCourse, targetDistance);
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
                    - std::max(roadCourse.totalLengthMeters,
                               roadCourse.visualLengthMeters));
    cameraTargetPositionX = targetSample.center.xMeters
            + targetForwardX * missingAhead;
    cameraTargetPositionZ = targetSample.center.zMeters
            + targetForwardZ * missingAhead;
    cameraTargetPositionY = targetSample.visualGroundElevationMeters()
            + cameraTargetHeightDistanceMeters;

    const WorkoutGameRoadSample riderSample =
            WorkoutGameRoadCourseBuilder::sample(roadCourse, distanceMeters);
    const auto applyPresentationCamera =
            [this, &riderSample, distanceMeters, lateralMeters]() {
        if (!riderSample.ready
                || cameraPresentationSnapshot.sideBlend <= 0.0) {
            return;
        }
        constexpr double SideDistanceMeters = 4.4;
        constexpr double SideBackMeters = 0.8;
        constexpr double SideHeightMeters = 2.65;
        constexpr double SideTargetHeightMeters = 0.88;
        constexpr double SideTargetAheadMeters = 0.12;
        const double forwardX = std::sin(
                riderSample.center.headingRadians);
        const double forwardZ = std::cos(
                riderSample.center.headingRadians);
        const double rightX = std::cos(
                riderSample.center.headingRadians);
        const double rightZ = -std::sin(
                riderSample.center.headingRadians);
        const double sideCameraX = riderPositionX
                - SideDistanceMeters * rightX
                - SideBackMeters * forwardX;
        const double sideCameraZ = riderPositionZ
                - SideDistanceMeters * rightZ
                - SideBackMeters * forwardZ;
        double sideGroundY = cameraGroundY;
        const WorkoutGame3DTerrainProfileSnapshot terrain =
                WorkoutGame3DTerrainProfile::build(
                    riderSample, distanceMeters, roadCourse.seed);
        if (terrain.ready) {
            sideGroundY = std::max(
                    sideGroundY,
                    WorkoutGame3DTerrainProfile::elevationAtLateral(
                        terrain, lateralMeters - SideDistanceMeters));
        }
        const double sideCameraY = sideGroundY + SideHeightMeters;
        const double sideTargetX = riderPositionX
                + SideTargetAheadMeters * forwardX;
        const double sideTargetZ = riderPositionZ
                + SideTargetAheadMeters * forwardZ;
        const double sideTargetY = riderPositionY + SideTargetHeightMeters;
        const double blend = std::clamp(
                cameraPresentationSnapshot.sideBlend, 0.0, 1.0);
        cameraPositionX += (sideCameraX - cameraPositionX) * blend;
        cameraPositionY += (sideCameraY - cameraPositionY) * blend;
        cameraPositionZ += (sideCameraZ - cameraPositionZ) * blend;
        cameraTargetPositionX +=
                (sideTargetX - cameraTargetPositionX) * blend;
        cameraTargetPositionY +=
                (sideTargetY - cameraTargetPositionY) * blend;
        cameraTargetPositionZ +=
                (sideTargetZ - cameraTargetPositionZ) * blend;
    };
    if (!riderSample.ready
            || riderSample.terrain != WorkoutGameTerrainKind::Berm
            || riderSample.pieceIndex >= roadCourse.pieces.size()) {
        applyPresentationCamera();
        constrainCameraTravel();
        return;
    }
    const WorkoutGameRoadPiece &piece =
            roadCourse.pieces[riderSample.pieceIndex];
    const WorkoutGameBermGeometryProfile berm =
            WorkoutGameBermGeometry::profile(piece.difficulty);
    const double local = distanceMeters
            - piece.geometryAnchorDistanceMeters;
    if (local <= berm.startMeters || local >= berm.endMeters) {
        applyPresentationCamera();
        constrainCameraTravel();
        return;
    }
    const double progress = std::clamp(
            (local - berm.startMeters) / (berm.endMeters - berm.startMeters),
            0.0, 1.0);
    const double blend = 0.72
            * std::pow(std::sin(Pi * progress), 2.0);
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
            WorkoutGameRoadCourseBuilder::sampleVisual(
                roadCourse,
                std::min(std::max(roadCourse.totalLengthMeters,
                                  roadCourse.visualLengthMeters),
                    distanceMeters + BermCameraLookAheadMeters));
    if (!bermTarget.ready) {
        applyPresentationCamera();
        constrainCameraTravel();
        return;
    }
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
    applyPresentationCamera();
    constrainCameraTravel();
}

QString WorkoutGame3DViewModel::cameraPresentation() const
{
    switch (cameraPresentationSnapshot.mode) {
    case WorkoutGame3DCameraPresentationMode::OpeningSide:
        return QStringLiteral("opening-side");
    case WorkoutGame3DCameraPresentationMode::IdleSide:
        return QStringLiteral("idle-side");
    case WorkoutGame3DCameraPresentationMode::ReturningToChase:
        return QStringLiteral("returning-to-chase");
    case WorkoutGame3DCameraPresentationMode::Chase:
        return QStringLiteral("chase");
    }
    return QStringLiteral("chase");
}

void WorkoutGame3DViewModel::setTelemetry(
        double watts,
        double targetWatts,
        int cadenceRpm,
        int heartRate,
        int virtualGear)
{
    const double nextWatts = std::max(0.0, finiteOrZero(watts));
    const double nextTargetWatts = std::max(
            0.0, finiteOrZero(targetWatts));
    const int nextCadenceRpm = std::clamp(cadenceRpm, 0, 300);
    const int nextHeartRate = std::clamp(heartRate, 0, 300);
    const int nextVirtualGear = std::max(1, virtualGear);
    if (currentWatts == nextWatts
            && currentTargetWatts == nextTargetWatts
            && currentCadenceRpm == nextCadenceRpm
            && currentHeartRate == nextHeartRate
            && currentVirtualGear == nextVirtualGear) {
        return;
    }
    currentWatts = nextWatts;
    currentTargetWatts = nextTargetWatts;
    currentCadenceRpm = nextCadenceRpm;
    currentHeartRate = nextHeartRate;
    currentVirtualGear = nextVirtualGear;
    emit telemetryChanged();
}

void WorkoutGame3DViewModel::setFps(double value)
{
    const double normalized = std::max(0.0, finiteOrZero(value));
    if (std::abs(normalized - currentFps) < 0.05) return;
    currentFps = normalized;
    emit fpsChanged();
}

void WorkoutGame3DViewModel::setDiagnostics(
        const WorkoutGameDiagnosticsSnapshot &snapshot)
{
    QString text;
    if (snapshot.ready) {
        const WorkoutGameDiagnosticsInput &input = snapshot.input;
        text = QStringLiteral(
                "P50 %1  P95 %2  P99 %3 MS   MAX %4   LATE %5   "
                "BACK %6   STILL %7   SKIP %8   QUEUE %9   WORK %10/%11 MS")
                .arg(input.p50FrameIntervalMs, 0, 'f', 1)
                .arg(input.p95FrameIntervalMs, 0, 'f', 1)
                .arg(input.p99FrameIntervalMs, 0, 'f', 1)
                .arg(snapshot.largestFrameIntervalMs)
                .arg(snapshot.lateFrameCount)
                .arg(snapshot.backwardFrameCount)
                .arg(snapshot.stationaryFrameCount)
                .arg(input.skippedSimulationTicks)
                .arg(input.rendererQueueDepth)
                .arg(input.presentationWorkMs, 0, 'f', 1)
                .arg(snapshot.largestPresentationWorkMs, 0, 'f', 1);
    }
    if (currentDiagnosticsText == text) return;
    currentDiagnosticsText = text;
    emit diagnosticsChanged();
}

int WorkoutGame3DViewModel::geometryQueueDepth() const
{
    return int(chunkBuilder.pendingDepth());
}

void WorkoutGame3DViewModel::setGeneratorState(const QString &state)
{
    if (currentGeneratorState == state) return;
    currentGeneratorState = state;
    emit generatorStateChanged();
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
    case WorkoutGameTerrainKind::Berm: return tr("Singletrack");
    case WorkoutGameTerrainKind::LogOver: return tr("Log over");
    case WorkoutGameTerrainKind::Tabletop: return tr("Tabletop");
    case WorkoutGameTerrainKind::RockSlab: return tr("Rock slab");
    case WorkoutGameTerrainKind::GapJump: return tr("Gap jump");
    }
    return tr("Trail");
}

QString WorkoutGame3DViewModel::gapJumpFeatureText(
        WorkoutGameGapJumpLine line,
        double predictedSpeedMetersPerSecond,
        bool locked)
{
    QString lineName;
    switch (line) {
    case WorkoutGameGapJumpLine::Short:
        lineName = tr("SHORT");
        break;
    case WorkoutGameGapJumpLine::Medium:
        lineName = tr("MEDIUM");
        break;
    case WorkoutGameGapJumpLine::Long:
        lineName = tr("LONG");
        break;
    case WorkoutGameGapJumpLine::None:
        return locked ? tr("Gap jump - SAFE LINE")
                      : tr("Gap jump - BUILD SPEED");
    }
    const double speedKph = std::max(
            0.0, finiteOrZero(predictedSpeedMetersPerSecond) * 3.6);
    return tr("Gap jump - %1 - %2 KM/H")
            .arg(lineName)
            .arg(speedKph, 0, 'f', 1);
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
        return feature.terrain == WorkoutGameTerrainKind::Climb
                    && feature.outcome == WorkoutGameFeatureOutcome::Bypassed
                ? tr("%1 complete - no bonus").arg(name)
                : feature.outcome == WorkoutGameFeatureOutcome::Completed
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
        return hud.terrain == WorkoutGameTerrainKind::GapJump
                ? tr("Preview") : tr("Prepare");
    case WorkoutGameFeatureHudState::Measure:
        return hud.terrain == WorkoutGameTerrainKind::Skinny
                ? tr("Hold target") : tr("Build power");
    case WorkoutGameFeatureHudState::Committed:
        return hud.route == WorkoutGameRoute::SafeBypass
                ? tr("Safe line") : tr("Line committed");
    case WorkoutGameFeatureHudState::ActNow:
        return hud.terrain == WorkoutGameTerrainKind::Skinny
                ? tr("Balance") : tr("Ride now");
    case WorkoutGameFeatureHudState::Complete:
        return tr("Complete");
    case WorkoutGameFeatureHudState::Bypass:
        return tr("Safe line");
    case WorkoutGameFeatureHudState::NoBonus:
        return tr("No bonus");
    case WorkoutGameFeatureHudState::Launch:
        return tr("Accelerate");
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

void WorkoutGame3DViewModel::rebuildFloor(
        double distanceMeters,
        bool immediate)
{
    if (!roadCourse.ready) {
        floorBuffers[0]->setCourse(roadCourse);
        floorBuffers[1]->setCourse(roadCourse);
        bermBuffers[0]->setCourse(roadCourse);
        bermBuffers[1]->setCourse(roadCourse);
        forestDressingBuffers[0]->setCourse(roadCourse);
        forestDressingBuffers[1]->setCourse(roadCourse);
        climbBuffers[0]->setCourse(roadCourse);
        climbBuffers[1]->setCourse(roadCourse);
        rootBuffers[0]->setCourse(roadCourse);
        rootBuffers[1]->setCourse(roadCourse);
        rockGardenBuffers[0]->setCourse(roadCourse);
        rockGardenBuffers[1]->setCourse(roadCourse);
        rockSlabBuffers[0]->setCourse(roadCourse);
        rockSlabBuffers[1]->setCourse(roadCourse);
        skinnyBuffers[0]->setCourse(roadCourse);
        skinnyBuffers[1]->setCourse(roadCourse);
        activeFloorBuffer = 0;
        floorBucket = std::numeric_limits<int>::min();
        requestedFloorBucket = std::numeric_limits<int>::min();
        updateVisibleTriangleCount();
        emit floorGeometryChanged();
        return;
    }
    const int bucket = int(std::floor(distanceMeters / FloorBucketMeters));
    if (bucket == requestedFloorBucket) return;
    requestedFloorBucket = bucket;
    const double start = std::max(
            0.0, distanceMeters - FloorBehindMeters);
    const double end = std::min(
            std::max(roadCourse.totalLengthMeters,
                     roadCourse.visualLengthMeters),
            distanceMeters + FloorAheadMeters);
    if (!immediate) {
        chunkBuilder.request(
                roadCourseSnapshot, start, end, bucket, courseGeneration);
        emit renderWorkChanged();
        return;
    }

    const int nextBuffer = 1 - activeFloorBuffer;
    const std::array<WorkoutGame3DGeometry::Layer,
                     WorkoutGame3DChunk::LayerCount> layers = {{
        WorkoutGame3DGeometry::Layer::ForestFloor,
        WorkoutGame3DGeometry::Layer::Berm,
        WorkoutGame3DGeometry::Layer::Roots,
        WorkoutGame3DGeometry::Layer::Climb,
        WorkoutGame3DGeometry::Layer::RockGarden,
        WorkoutGame3DGeometry::Layer::RockSlab,
        WorkoutGame3DGeometry::Layer::Skinny,
        WorkoutGame3DGeometry::Layer::ForestDressing
    }};
    std::array<WorkoutGame3DGeometry *, WorkoutGame3DChunk::LayerCount>
            targets = {{
        floorBuffers[std::size_t(nextBuffer)].get(),
        bermBuffers[std::size_t(nextBuffer)].get(),
        rootBuffers[std::size_t(nextBuffer)].get(),
        climbBuffers[std::size_t(nextBuffer)].get(),
        rockGardenBuffers[std::size_t(nextBuffer)].get(),
        rockSlabBuffers[std::size_t(nextBuffer)].get(),
        skinnyBuffers[std::size_t(nextBuffer)].get(),
        forestDressingBuffers[std::size_t(nextBuffer)].get()
    }};
    for (std::size_t index = 0; index < targets.size(); ++index) {
        targets[index]->setMeshData(WorkoutGame3DGeometry::buildMeshData(
                layers[index], roadCourse, start, end));
    }
    if (!targets[0]->ready()) return;
    activeFloorBuffer = nextBuffer;
    floorBucket = bucket;
    updateVisibleTriangleCount();
    emit floorGeometryChanged();
}

void WorkoutGame3DViewModel::installReadyFloorChunk()
{
    WorkoutGame3DChunk chunk;
    if (!chunkBuilder.takeLatest(chunk)) return;
    emit renderWorkChanged();
    if (chunk.courseGeneration != courseGeneration
            || chunk.bucket != requestedFloorBucket
            || !chunk.floorReady()) {
        return;
    }
    const int nextBuffer = 1 - activeFloorBuffer;
    std::array<WorkoutGame3DGeometry *, WorkoutGame3DChunk::LayerCount>
            targets = {{
        floorBuffers[std::size_t(nextBuffer)].get(),
        bermBuffers[std::size_t(nextBuffer)].get(),
        rootBuffers[std::size_t(nextBuffer)].get(),
        climbBuffers[std::size_t(nextBuffer)].get(),
        rockGardenBuffers[std::size_t(nextBuffer)].get(),
        rockSlabBuffers[std::size_t(nextBuffer)].get(),
        skinnyBuffers[std::size_t(nextBuffer)].get(),
        forestDressingBuffers[std::size_t(nextBuffer)].get()
    }};
    for (std::size_t index = 0; index < targets.size(); ++index) {
        targets[index]->setMeshData(chunk.layers[index]);
    }
    activeFloorBuffer = nextBuffer;
    floorBucket = chunk.bucket;
    updateVisibleTriangleCount();
    emit floorGeometryChanged();
}

void WorkoutGame3DViewModel::scheduleReadyFloorChunk()
{
    if (floorInstallQueued.exchange(true, std::memory_order_acq_rel)) return;
    const bool queued = QMetaObject::invokeMethod(
            this,
            [this]() {
                installReadyFloorChunk();
                floorInstallQueued.store(false, std::memory_order_release);
                if (chunkBuilder.resultDepth() > 0) {
                    scheduleReadyFloorChunk();
                }
            },
            Qt::QueuedConnection);
    if (!queued) {
        floorInstallQueued.store(false, std::memory_order_release);
    }
}

void WorkoutGame3DViewModel::updateVisibleTriangleCount()
{
    const std::size_t active = std::size_t(activeFloorBuffer);
    int triangles = trail->triangleCount()
            + bermBuffers[active]->triangleCount()
            + bypass->triangleCount()
            + gapJump->triangleCount();
    triangles += floorBuffers[active]->triangleCount();
    triangles += rootBuffers[active]->triangleCount();
    triangles += climbBuffers[active]->triangleCount();
    triangles += rockGardenBuffers[active]->triangleCount();
    triangles += rockSlabBuffers[active]->triangleCount();
    triangles += skinnyBuffers[active]->triangleCount();
    triangles += forestDressingBuffers[active]->triangleCount();
    if (triangles == currentVisibleTriangles) return;
    currentVisibleTriangles = triangles;
    emit renderWorkChanged();
}

void WorkoutGame3DViewModel::rebuildTrees(double distanceMeters)
{
    if (!roadCourse.ready) return;
    const int bucket = int(std::floor(distanceMeters / TreeSpacingMeters));
    const bool sidePresentation =
            cameraPresentationSnapshot.sideBlend > 0.0;
    const int presentationBucket = sidePresentation
            ? int(std::floor(distanceMeters))
            : std::numeric_limits<int>::min();
    if (bucket == treeBucket
            && presentationBucket == treePresentationBucket) {
        return;
    }
    treeBucket = bucket;
    treePresentationBucket = presentationBucket;
    visibleTrees.clear();
    for (int offset = -3; offset <= 15; ++offset) {
        const int slot = bucket + offset;
        if (slot < 0) continue;
        for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
            const std::uint32_t random = mix(
                    roadCourse.seed
                    ^ std::uint32_t(slot * 0x9e3779b9u)
                    ^ std::uint32_t((sideIndex + 1) * 0x85ebca6bu));
            const double jitter =
                    (double(random & 255u) / 255.0 - 0.5) * 2.2;
            const double distance = (double(slot) + 0.5)
                    * TreeSpacingMeters + jitter;
            if (distance < 0.0) continue;
            if (distance > std::max(roadCourse.totalLengthMeters,
                                    roadCourse.visualLengthMeters)) break;
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sampleVisual(
                        roadCourse, distance);
            if (!sample.ready) continue;
            const double side = sideIndex == 0 ? -1.0 : 1.0;
            const double lateral = side * (
                    3.0 + double((random >> 8) & 255u) / 110.0);
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
            const double treeY =
                    WorkoutGame3DTerrainProfile::elevationAtLateral(
                        terrain, lateral);
            const double requiredClearance = crownRadius
                    + CameraCorridorClearanceMeters
                    + (sidePresentation ? 1.05 : 0.0);
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
            if (visibleTrees.size() >= MaximumVisibleTrees) break;
        }
        if (visibleTrees.size() >= MaximumVisibleTrees) break;
    }
    emit treesChanged();
}
