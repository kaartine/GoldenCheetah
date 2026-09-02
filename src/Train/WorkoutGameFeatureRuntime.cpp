/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameFeatureRuntime.h"

#include "WorkoutGameBermGeometry.h"
#include "WorkoutGameClimbGeometry.h"
#include "WorkoutGameTrailBranch.h"

#include "WorkoutGameFeatureGeometry.h"
#include "WorkoutGameGapJumpGeometry.h"
#include "WorkoutGameRockGardenGeometry.h"
#include "WorkoutGameRockSlabGeometry.h"
#include "WorkoutGameRootGeometry.h"
#include "WorkoutGameSkinnyGeometry.h"
#include "WorkoutGameTabletopGeometry.h"

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
    const double cubed = clamped * clamped * clamped;
    return cubed * (clamped * (clamped * 6.0 - 15.0) + 10.0);
}

double jumpArc(double progress)
{
    constexpr double ApexProgress = 0.30;
    const double clamped = std::clamp(progress, 0.0, 1.0);
    if (clamped <= ApexProgress) {
        return std::sin(clamped / ApexProgress * Pi * 0.5);
    }
    return std::sin((1.0 - clamped) / (1.0 - ApexProgress) * Pi * 0.5);
}

WorkoutGameFeatureMotion motionFor(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver:
    case WorkoutGameTerrainKind::Tabletop:
    case WorkoutGameTerrainKind::GapJump:
        return WorkoutGameFeatureMotion::Jump;
    case WorkoutGameTerrainKind::Roots:
    case WorkoutGameTerrainKind::RockGarden:
    case WorkoutGameTerrainKind::RockSlab:
        return WorkoutGameFeatureMotion::Absorb;
    case WorkoutGameTerrainKind::Skinny:
        return WorkoutGameFeatureMotion::Balance;
    case WorkoutGameTerrainKind::Climb:
        return WorkoutGameFeatureMotion::Climb;
    case WorkoutGameTerrainKind::Drop:
        return WorkoutGameFeatureMotion::Drop;
    default:
        return WorkoutGameFeatureMotion::None;
    }
}

double gapJumpLateralAt(
        double distanceMeters,
        double splitStartMeters,
        double splitLengthMeters,
        double mergeStartMeters,
        double mergeEndMeters,
        double lateralMeters)
{
    if (distanceMeters <= splitStartMeters
            || distanceMeters >= mergeEndMeters) {
        return 0.0;
    }
    const double splitEndMeters = splitStartMeters
            + std::max(0.01, splitLengthMeters);
    if (distanceMeters < splitEndMeters) {
        return lateralMeters * smootherStep(
                (distanceMeters - splitStartMeters)
                    / (splitEndMeters - splitStartMeters));
    }
    if (distanceMeters <= mergeStartMeters) return lateralMeters;
    return lateralMeters * (1.0 - smootherStep(
            (distanceMeters - mergeStartMeters)
                / std::max(0.01, mergeEndMeters - mergeStartMeters)));
}

std::uint64_t gapJumpActionId(
        std::uint64_t baseActionId,
        WorkoutGameGapJumpLine line)
{
    constexpr std::uint64_t LineMask = std::uint64_t(0x3) << 8;
    return (baseActionId & ~LineMask)
            | (std::uint64_t(line) << 8);
}

const WorkoutGameRoadGapJumpLine *gapJumpRoadLine(
        const WorkoutGameRoadGapJumpGate &gate,
        WorkoutGameGapJumpLine id)
{
    if (!gate.enabled || id == WorkoutGameGapJumpLine::None) return nullptr;
    for (const WorkoutGameRoadGapJumpLine &line : gate.lines) {
        if (line.id == id) return &line;
    }
    return nullptr;
}

double jumpHeight(WorkoutGameTerrainKind terrain)
{
    if (terrain == WorkoutGameTerrainKind::Tabletop) return 1.55;
    if (terrain == WorkoutGameTerrainKind::BunnyHop) return 0.58;
    return 0.72;
}

double jumpFlightDurationSeconds(
        WorkoutGameTerrainKind terrain,
        double difficulty,
        double speedKph)
{
    const double challenge = std::clamp(difficulty, 0.0, 1.0);
    const double speedBonus = std::clamp(
            (speedKph - 25.0) / 20.0, 0.0, 1.0);
    if (terrain == WorkoutGameTerrainKind::Tabletop) {
        const double forwardSpeed = std::max(0.0, speedKph) / 3.6;
        return WorkoutGameTabletopGeometry::profile(
                challenge).flightDurationSeconds(forwardSpeed);
    }
    if (terrain == WorkoutGameTerrainKind::BunnyHop) {
        return 0.65 + 0.25 * challenge + 0.15 * speedBonus;
    }
    return 0.9 + 0.5 * challenge + 0.4 * speedBonus;
}

double minimumJumpTravelMeters(WorkoutGameTerrainKind terrain)
{
    return terrain == WorkoutGameTerrainKind::BunnyHop ? 3.2 : 5.5;
}

}

bool WorkoutGameFeatureRuntime::configure(
        const WorkoutGameRoadCourse &course)
{
    reset();
    if (!course.ready || course.pieces.empty()
            || !std::isfinite(course.totalLengthMeters)
            || course.totalLengthMeters <= 0.0) {
        return false;
    }
    configuredCourse = course;
    std::size_t sectionCount = 0;
    for (const WorkoutGameRoadPiece &piece : configuredCourse.pieces) {
        sectionCount = std::max(sectionCount, piece.sourceSectionIndex + 1);
    }
    sections.resize(sectionCount);
    for (std::size_t pieceIndex = 0;
         pieceIndex < configuredCourse.pieces.size(); ++pieceIndex) {
        const WorkoutGameRoadPiece &piece = configuredCourse.pieces[pieceIndex];
        SectionLayout &layout = sections[piece.sourceSectionIndex];
        if (!layout.valid) {
            layout.valid = true;
            layout.terrain = piece.terrain;
            layout.startDistanceMeters = piece.startDistanceMeters;
            layout.terrainPieceIndex = pieceIndex;
        }
        layout.endDistanceMeters = piece.startDistanceMeters + piece.lengthMeters;
        if (piece.challenge.enabled) {
            layout.challengePieceIndex = pieceIndex;
        }
    }
    for (const WorkoutGameRoadTimelineSection &timeline :
            configuredCourse.timeline) {
        if (timeline.sourceSectionIndex < sections.size()) {
            sections[timeline.sourceSectionIndex].durationMs =
                    timeline.durationMs;
        }
    }
    return true;
}

bool WorkoutGameFeatureRuntime::airborneExpected(
        const WorkoutGameFeatureRuntimeSnapshot &feature)
{
    if (!feature.ready) return false;
    if (feature.terrain == WorkoutGameTerrainKind::GapJump) {
        return feature.lockedGapLine != WorkoutGameGapJumpLine::None
                && feature.motion == WorkoutGameFeatureMotion::Jump
                && feature.phase == WorkoutGameFeaturePhase::Action;
    }
    if (feature.motion == WorkoutGameFeatureMotion::Jump
            && feature.phase == WorkoutGameFeaturePhase::Committed) {
        return feature.visualDistanceMeters
                >= feature.physicalTakeoffDistanceMeters;
    }
    return (feature.motion == WorkoutGameFeatureMotion::Jump
                || feature.motion == WorkoutGameFeatureMotion::Drop)
            && (feature.phase == WorkoutGameFeaturePhase::Action
                    || feature.phase == WorkoutGameFeaturePhase::Recovery);
}

void WorkoutGameFeatureRuntime::reset()
{
    configuredCourse = WorkoutGameRoadCourse();
    sections.clear();
    gapJumpState = GapJumpState();
}

WorkoutGameFeatureRuntimeSnapshot WorkoutGameFeatureRuntime::update(
        const WorkoutGameSimulationSnapshot &simulation,
        double actualWatts,
        double targetWatts)
{
    WorkoutGameFeatureRuntimeSnapshot result;
    if (!configuredCourse.ready || !simulation.ready
            || simulation.activeSection < 0
            || simulation.activeSection >= int(sections.size())) {
        return result;
    }
    const SectionLayout &activeLayout = sections[simulation.activeSection];
    if (!activeLayout.valid) return result;

    const double sectionProgress = std::clamp(
            std::isfinite(simulation.sectionProgress)
                    ? simulation.sectionProgress : 0.0,
            0.0, 1.0);
    const double visualDistanceMeters = activeLayout.startDistanceMeters
            + (activeLayout.endDistanceMeters
                - activeLayout.startDistanceMeters) * sectionProgress;
    int featureSection = simulation.activeSection;
    WorkoutGameFeatureOutcome featureOutcome = simulation.featureOutcome;
    double featureReadiness = simulation.challengeReadiness;
    const SectionLayout *layout = &activeLayout;
    if (activeLayout.challengePieceIndex
                >= configuredCourse.pieces.size()
            && simulation.previousFeatureSection >= 0
            && simulation.previousFeatureSection < int(sections.size())) {
        const SectionLayout &previous =
                sections[std::size_t(simulation.previousFeatureSection)];
        if (previous.valid
                && previous.challengePieceIndex
                    < configuredCourse.pieces.size()
                && configuredCourse.pieces[previous.challengePieceIndex]
                        .terrain == WorkoutGameTerrainKind::Climb
                && visualDistanceMeters
                    <= previous.endDistanceMeters + 6.0) {
            featureSection = simulation.previousFeatureSection;
            featureOutcome = simulation.previousFeatureOutcome;
            featureReadiness = simulation.previousFeatureReadiness;
            layout = &previous;
        }
    }

    result.ready = true;
    result.sourceSectionIndex = featureSection;
    result.terrain = layout->terrain;
    result.visualDistanceMeters = visualDistanceMeters;
    result.outcome = featureOutcome;
    result.route = simulation.route;
    result.readiness = std::clamp(
            std::isfinite(featureReadiness) ? featureReadiness : 0.0,
            0.0, 1.0);

    if (layout->challengePieceIndex >= configuredCourse.pieces.size()) {
        if (layout->terrain == WorkoutGameTerrainKind::Berm
                && layout->terrainPieceIndex < configuredCourse.pieces.size()) {
            result.outcome = WorkoutGameFeatureOutcome::None;
            result.route = WorkoutGameRoute::MainLine;
            result.readiness = 0.0;
            const WorkoutGameRoadPiece &piece =
                    configuredCourse.pieces[layout->terrainPieceIndex];
            const WorkoutGameBermGeometryProfile berm =
                    WorkoutGameBermGeometry::profile(piece.difficulty);
            const double local = result.visualDistanceMeters
                    - piece.geometryAnchorDistanceMeters;
            result.obstacleDistanceMeters =
                    piece.geometryAnchorDistanceMeters;
            result.bermLineBias = berm.effortLineBias(
                    actualWatts, targetWatts);
            result.lateralOffsetMeters = berm.effortLineLateralMeters(
                    local, piece.turnRadians, result.bermLineBias);
        }
        return result;
    }
    const WorkoutGameRoadPiece *piece =
            &configuredCourse.pieces[layout->challengePieceIndex];
    result.terrain = piece->terrain;
    if (result.terrain == WorkoutGameTerrainKind::Rollers
            || result.terrain == WorkoutGameTerrainKind::Climb) {
        result.route = WorkoutGameRoute::MainLine;
    }
    result.motion = motionFor(piece->terrain);
    result.prepareDistanceMeters = piece->challenge.prepareDistanceMeters;
    result.decisionDistanceMeters = piece->challenge.decisionDistanceMeters;
    result.obstacleDistanceMeters = piece->challenge.obstacleDistanceMeters;
    result.physicalTakeoffDistanceMeters = result.obstacleDistanceMeters;
    result.distanceToObstacleMeters = result.obstacleDistanceMeters
            - result.visualDistanceMeters;

    if (result.motion == WorkoutGameFeatureMotion::Jump
            && piece->terrain != WorkoutGameTerrainKind::GapJump) {
        const WorkoutGameFeatureGeometryProfile geometry =
                WorkoutGameFeatureGeometry::profile(
                    piece->terrain, piece->difficulty);
        if (geometry.ready) {
            const double takeoffOffset = piece->terrain
                    == WorkoutGameTerrainKind::Tabletop
                ? geometry.plateauStartMeters : geometry.startMeters;
            result.physicalTakeoffDistanceMeters += takeoffOffset;
        }
    }
    double actionStart = result.obstacleDistanceMeters;
    double actionEnd = std::min(
            layout->endDistanceMeters, actionStart + 6.0);
    bool jumpSpeedSupported = true;
    if (piece->terrain == WorkoutGameTerrainKind::GapJump) {
        const WorkoutGameGapJumpGeometryProfile profile =
                WorkoutGameGapJumpGeometry::profile(piece->difficulty);
        const WorkoutGameRoadGapJumpGate &gate = piece->gapJump;
        const std::uint64_t baseActionId =
                (std::uint64_t(featureSection + 1) << 32)
                | (std::uint64_t(piece->terrain) + 1u);
        const bool newAction = gapJumpState.sourceSectionIndex != featureSection
                || gapJumpState.baseActionId != baseActionId
                || (gapJumpState.hasTimestamp
                    && simulation.workoutTimeMs
                        < gapJumpState.lastWorkoutTimeMs);
        if (newAction) {
            gapJumpState = GapJumpState();
            gapJumpState.sourceSectionIndex = featureSection;
            gapJumpState.baseActionId = baseActionId;
            gapJumpState.selector = WorkoutGameGapJumpSelector(profile);
        }

        const double predictedSpeed = simulation.challengeMeasurementActive
                && std::isfinite(
                    simulation.challengeMetrics.averageSpeedKph)
                && simulation.challengeMetrics.averageSpeedKph >= 0.0
            ? simulation.challengeMetrics.averageSpeedKph / 3.6
            : std::max(0.0, std::isfinite(simulation.speedKph)
                ? simulation.speedKph / 3.6 : 0.0);
        result.predictedApproachSpeedMetersPerSecond = predictedSpeed;

        int elapsedMilliseconds = 50;
        if (gapJumpState.hasTimestamp) {
            elapsedMilliseconds = int(std::clamp<std::int64_t>(
                    simulation.workoutTimeMs
                        - gapJumpState.lastWorkoutTimeMs,
                    0, 2000));
        }
        gapJumpState.lastWorkoutTimeMs = simulation.workoutTimeMs;
        gapJumpState.hasTimestamp = true;
        if (!gapJumpState.selector.state().locked
                && elapsedMilliseconds > 0) {
            for (int remaining = elapsedMilliseconds;
                 remaining > 0; remaining -= 50) {
                gapJumpState.selector.update(
                        predictedSpeed, std::min(50, remaining));
            }
        }

        result.prepareDistanceMeters = gate.enabled
                ? gate.prepareDistanceMeters
                : piece->challenge.prepareDistanceMeters;
        result.decisionDistanceMeters = gate.enabled
                ? gate.lockDistanceMeters
                : piece->challenge.decisionDistanceMeters;
        result.distanceToObstacleMeters = result.obstacleDistanceMeters
                - result.visualDistanceMeters;

        if (!gapJumpState.selector.state().locked
                && result.visualDistanceMeters
                    >= result.decisionDistanceMeters) {
            const double measuredActual =
                    simulation.challengeMeasurementActive
                        ? simulation.challengeMetrics.averageActualWatts
                        : actualWatts;
            const double measuredTarget =
                    simulation.challengeMeasurementActive
                        ? simulation.challengeMetrics.averageTargetWatts
                        : targetWatts;
            const bool finiteTelemetry = std::isfinite(measuredActual)
                    && std::isfinite(measuredTarget)
                    && std::isfinite(predictedSpeed)
                    && measuredActual >= 0.0
                    && measuredTarget > 0.0;
            const bool effortReady = finiteTelemetry
                    && measuredActual >= measuredTarget;
            const bool telemetryFresh = finiteTelemetry
                    && (simulation.challengeMeasurementActive
                        || actualWatts > 0.0);
            gapJumpState.selector.lock(
                    baseActionId, effortReady && gate.enabled,
                    telemetryFresh);
        }

        const WorkoutGameGapJumpSelectionState selection =
                gapJumpState.selector.state();
        result.provisionalGapLine = selection.provisionalLine;
        result.lockedGapLine = selection.lockedLine;
        result.gapLineLocked = selection.locked;
        const WorkoutGameGapJumpLine activeLine = selection.locked
                ? selection.lockedLine : selection.provisionalLine;
        const WorkoutGameRoadGapJumpLine *line =
                gapJumpRoadLine(gate, activeLine);
        const bool bypassGap = selection.locked
                && selection.lockedLine == WorkoutGameGapJumpLine::None;
        if (bypassGap) {
            result.route = WorkoutGameRoute::SafeBypass;
            result.outcome = WorkoutGameFeatureOutcome::Bypassed;
        } else {
            result.route = WorkoutGameRoute::MainLine;
            if (selection.locked) {
                result.outcome = WorkoutGameFeatureOutcome::Completed;
            }
        }

        const WorkoutGameRoadGapJumpLine *fallbackLine = gate.enabled
                ? &gate.lines.front() : nullptr;
        actionStart = line ? line->takeoffDistanceMeters
                : fallbackLine ? fallbackLine->takeoffDistanceMeters
                : result.obstacleDistanceMeters;
        actionEnd = std::min(
                layout->endDistanceMeters,
                line ? line->landingDistanceMeters
                     : fallbackLine ? fallbackLine->landingDistanceMeters
                     : actionStart);
        result.physicalTakeoffDistanceMeters = actionStart;
        result.selectedGapLengthMeters = line ? line->gapLengthMeters : 0.0;
        const double sectionLengthMeters = std::max(
                0.01, layout->endDistanceMeters - layout->startDistanceMeters);
        const double sectionSeconds = std::max(
                0.001, double(layout->durationMs) / 1000.0);
        const double courseSpeedMetersPerSecond = layout->durationMs > 0
                ? sectionLengthMeters / sectionSeconds
                : std::max(0.1, predictedSpeed);
        result.flightDurationSeconds = line
                ? std::clamp(
                    line->gapLengthMeters / courseSpeedMetersPerSecond,
                    0.25,
                    std::min({line->nominalFlightSeconds, 2.0,
                              profile.maximumFlightSeconds}))
                : 0.0;

        const double mergeEnd = gate.enabled
                ? gate.mergeEndDistanceMeters : actionEnd;
        const double mergeStart = std::max(
                actionEnd,
                mergeEnd - profile.mergeLengthMeters);
        const double lateral = bypassGap
                ? piece->challenge.bypassLateralMeters
                : line ? line->lateralMeters : 0.0;
        if (selection.locked) {
            result.lateralOffsetMeters = gapJumpLateralAt(
                    result.visualDistanceMeters,
                    gate.enabled ? gate.splitStartDistanceMeters
                                 : result.decisionDistanceMeters,
                    profile.splitLengthMeters,
                    mergeStart, mergeEnd, lateral);
        }
        result.actionId = gapJumpActionId(baseActionId,
                                         selection.lockedLine);
    } else if (piece->terrain == WorkoutGameTerrainKind::Climb) {
        const WorkoutGameClimbGeometryProfile climb =
                WorkoutGameClimbGeometry::profile(piece->difficulty);
        actionStart = std::max(
                layout->startDistanceMeters,
                result.obstacleDistanceMeters + climb.activeStartMeters);
        actionEnd = std::min(
                layout->endDistanceMeters,
                result.obstacleDistanceMeters + climb.endMeters);
    } else if (piece->terrain == WorkoutGameTerrainKind::Roots) {
        const WorkoutGameRootGeometryProfile roots =
                WorkoutGameRootGeometry::profile(piece->difficulty);
        actionStart = std::max(
                layout->startDistanceMeters,
                result.obstacleDistanceMeters + roots.activeStartMeters);
        actionEnd = std::min(
                layout->endDistanceMeters,
                result.obstacleDistanceMeters + roots.activeEndMeters);
    } else if (piece->terrain == WorkoutGameTerrainKind::RockGarden) {
        const WorkoutGameRockGardenGeometryProfile rocks =
                WorkoutGameRockGardenGeometry::profile(piece->difficulty);
        actionStart = std::max(
                layout->startDistanceMeters,
                result.obstacleDistanceMeters + rocks.activeStartMeters);
        actionEnd = std::min(
                layout->endDistanceMeters,
                result.obstacleDistanceMeters + rocks.activeEndMeters);
    } else if (piece->terrain == WorkoutGameTerrainKind::RockSlab) {
        const WorkoutGameRockSlabGeometryProfile slab =
                WorkoutGameRockSlabGeometry::profile(piece->difficulty);
        actionStart = std::max(
                layout->startDistanceMeters,
                result.obstacleDistanceMeters + slab.activeStartMeters);
        actionEnd = std::min(
                layout->endDistanceMeters,
                result.obstacleDistanceMeters + slab.activeEndMeters);
    } else if (piece->terrain == WorkoutGameTerrainKind::Skinny) {
        const WorkoutGameSkinnyGeometryProfile skinny =
                WorkoutGameSkinnyGeometry::profile(piece->difficulty);
        actionStart = std::max(
                layout->startDistanceMeters,
                result.obstacleDistanceMeters + skinny.activeStartMeters);
        actionEnd = std::min(
                layout->endDistanceMeters,
                result.obstacleDistanceMeters + skinny.activeEndMeters);
    } else if (result.motion == WorkoutGameFeatureMotion::Jump) {
        const WorkoutGameFeatureGeometryProfile geometry =
                WorkoutGameFeatureGeometry::profile(
                    piece->terrain, piece->difficulty);
        const double sectionLengthMeters = std::max(
                0.01, layout->endDistanceMeters - layout->startDistanceMeters);
        const double sectionSeconds = std::max(
                0.001, double(layout->durationMs) / 1000.0);
        const double timelineMetersPerSecond = layout->durationMs > 0
                ? sectionLengthMeters / sectionSeconds
                : std::max(3.0, simulation.speedKph / 3.6);
        const double flightSpeedKph = piece->terrain
                    == WorkoutGameTerrainKind::Tabletop
                ? timelineMetersPerSecond * 3.6
                : std::max(0.0, simulation.speedKph);
        if (piece->terrain == WorkoutGameTerrainKind::Tabletop) {
            jumpSpeedSupported = WorkoutGameTabletopGeometry::profile(
                    piece->difficulty).supportsJumpAtForwardSpeed(
                        timelineMetersPerSecond);
        }
        const double requestedFlightSeconds = jumpFlightDurationSeconds(
                piece->terrain, piece->difficulty, flightSpeedKph);
        actionStart = result.physicalTakeoffDistanceMeters;
        if (piece->terrain == WorkoutGameTerrainKind::Tabletop) {
            actionEnd = std::min(
                    layout->endDistanceMeters,
                    actionStart + timelineMetersPerSecond
                        * requestedFlightSeconds);
            result.flightDurationSeconds = requestedFlightSeconds;
        } else {
            actionEnd = std::min(
                    layout->endDistanceMeters,
                    std::max({actionStart + minimumJumpTravelMeters(
                                        piece->terrain),
                             result.obstacleDistanceMeters
                                + geometry.endMeters + 1.5,
                             actionStart + timelineMetersPerSecond
                                * requestedFlightSeconds}));
            result.flightDurationSeconds = std::clamp(
                    (actionEnd - actionStart) / timelineMetersPerSecond,
                    0.0, 5.0);
        }
    }
    result.actionStartDistanceMeters = actionStart;
    result.actionEndDistanceMeters = actionEnd;
    if (piece->terrain == WorkoutGameTerrainKind::Climb
            && result.visualDistanceMeters >= actionStart
            && result.visualDistanceMeters < actionEnd) {
        result.phase = WorkoutGameFeaturePhase::Action;
    } else if (result.visualDistanceMeters
            < result.prepareDistanceMeters) {
        result.phase = WorkoutGameFeaturePhase::Approach;
    } else if (result.visualDistanceMeters
            < result.decisionDistanceMeters) {
        result.phase = WorkoutGameFeaturePhase::Measure;
    } else if (result.visualDistanceMeters < actionStart) {
        result.phase = WorkoutGameFeaturePhase::Committed;
    } else if (result.visualDistanceMeters < actionEnd) {
        result.phase = WorkoutGameFeaturePhase::Action;
    } else {
        result.phase = WorkoutGameFeaturePhase::Recovery;
    }

    if (piece->terrain != WorkoutGameTerrainKind::GapJump) {
        result.actionId = (std::uint64_t(featureSection + 1) << 32)
                | (std::uint64_t(piece->terrain) + 1u);
    }
    const bool completed = result.outcome
            == WorkoutGameFeatureOutcome::Completed;
    const bool bypass = result.terrain != WorkoutGameTerrainKind::Rollers
            && result.terrain != WorkoutGameTerrainKind::Climb
            && result.terrain != WorkoutGameTerrainKind::Skinny
            && (result.outcome == WorkoutGameFeatureOutcome::Bypassed
                || result.route == WorkoutGameRoute::SafeBypass);
    if (bypass && piece->terrain != WorkoutGameTerrainKind::GapJump) {
        if (piece->terrain == WorkoutGameTerrainKind::Roots) {
            const WorkoutGameRootGeometryProfile roots =
                    WorkoutGameRootGeometry::profile(piece->difficulty);
            result.lateralOffsetMeters = roots.safeLineOffsetMeters(
                        result.visualDistanceMeters
                            - piece->challenge.obstacleDistanceMeters);
        } else if (piece->terrain == WorkoutGameTerrainKind::RockGarden) {
            const WorkoutGameRockGardenGeometryProfile rocks =
                    WorkoutGameRockGardenGeometry::profile(
                        piece->difficulty);
            result.lateralOffsetMeters = rocks.safeLineOffsetMeters(
                    result.visualDistanceMeters
                        - piece->challenge.obstacleDistanceMeters);
        } else if (piece->terrain == WorkoutGameTerrainKind::RockSlab) {
            const WorkoutGameRockSlabGeometryProfile slab =
                    WorkoutGameRockSlabGeometry::profile(
                        piece->difficulty);
            result.lateralOffsetMeters = slab.safeLineOffsetMeters(
                    result.visualDistanceMeters
                        - piece->challenge.obstacleDistanceMeters);
        } else {
            const double branchStart =
                    piece->challenge.bypassStartDistanceMeters;
            const double branchEnd = std::max(
                    branchStart + 0.01,
                    piece->challenge.bypassEndDistanceMeters);
            result.lateralOffsetMeters = WorkoutGameTrailBranch::lateralAt(
                    result.visualDistanceMeters,
                    branchStart, branchEnd,
                    piece->challenge.bypassLateralMeters);
        }
    }

    if (result.phase == WorkoutGameFeaturePhase::Action) {
        const double actionProgress = std::clamp(
                (result.visualDistanceMeters - actionStart)
                    / std::max(0.01,
                        actionEnd - actionStart),
                0.0, 1.0);
        if (result.motion == WorkoutGameFeatureMotion::Jump && completed
                && jumpSpeedSupported
                && !bypass) {
            result.triggerJump = true;
            double heightMeters = jumpHeight(piece->terrain);
            double arcProgress = actionProgress;
            if (piece->terrain == WorkoutGameTerrainKind::GapJump) {
                const auto *line = gapJumpRoadLine(
                        piece->gapJump, result.lockedGapLine);
                if (line) {
                    heightMeters = std::min(
                            2.4,
                            line->lipHeightMeters
                                + 0.12 * line->gapLengthMeters);
                    constexpr double ApexProgress = 0.35;
                    arcProgress = actionProgress <= ApexProgress
                            ? actionProgress / ApexProgress * 0.30
                            : 0.30 + (actionProgress - ApexProgress)
                                / (1.0 - ApexProgress) * 0.70;
                }
            }
            result.verticalOffsetMeters = heightMeters
                    * jumpArc(arcProgress);
            result.pitchDegrees = 7.0 * std::cos(Pi * actionProgress);
        } else if (result.motion == WorkoutGameFeatureMotion::Drop
                && completed && !bypass) {
            result.pitchDegrees = -14.0 * std::sin(Pi * actionProgress);
            const WorkoutGameFeatureGeometryProfile geometry =
                    WorkoutGameFeatureGeometry::profile(
                        piece->terrain, piece->difficulty);
            result.verticalOffsetMeters = geometry.heightMeters
                    * smoothStep(actionProgress);
        }
    }
    return result;
}
