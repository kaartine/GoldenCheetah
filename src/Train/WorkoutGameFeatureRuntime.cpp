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
#include "WorkoutGameRockGardenGeometry.h"
#include "WorkoutGameRockSlabGeometry.h"
#include "WorkoutGameRootGeometry.h"
#include "WorkoutGameSkinnyGeometry.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double Pi = 3.14159265358979323846;

double smoothStep(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    return clamped * clamped * (3.0 - 2.0 * clamped);
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

double jumpHeight(WorkoutGameTerrainKind terrain)
{
    if (terrain == WorkoutGameTerrainKind::Tabletop) return 1.35;
    if (terrain == WorkoutGameTerrainKind::BunnyHop) return 0.42;
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
        return 1.7 + 0.8 * challenge + 2.2 * speedBonus;
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
}

WorkoutGameFeatureRuntimeSnapshot WorkoutGameFeatureRuntime::update(
        const WorkoutGameSimulationSnapshot &simulation) const
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

    if (result.motion == WorkoutGameFeatureMotion::Jump) {
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
    if (piece->terrain == WorkoutGameTerrainKind::Climb) {
        const WorkoutGameClimbGeometryProfile climb =
                WorkoutGameClimbGeometry::profile(piece->difficulty);
        actionStart = std::max(
                layout->startDistanceMeters,
                result.obstacleDistanceMeters + climb.activeStartMeters);
        actionEnd = std::min(
                layout->endDistanceMeters,
                result.obstacleDistanceMeters + climb.endMeters);
    } else if (piece->terrain == WorkoutGameTerrainKind::Berm) {
        const WorkoutGameBermGeometryProfile berm =
                WorkoutGameBermGeometry::profile(piece->difficulty);
        actionStart = std::max(
                layout->startDistanceMeters,
                result.obstacleDistanceMeters + berm.startMeters);
        actionEnd = std::min(
                layout->endDistanceMeters,
                result.obstacleDistanceMeters + berm.endMeters);
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
        const double requestedFlightSeconds = jumpFlightDurationSeconds(
                piece->terrain, piece->difficulty,
                std::max(0.0, simulation.speedKph));
        actionStart = result.physicalTakeoffDistanceMeters;
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
    result.actionStartDistanceMeters = actionStart;
    result.actionEndDistanceMeters = actionEnd;
    if (piece->terrain == WorkoutGameTerrainKind::Climb
            && result.visualDistanceMeters >= actionStart
            && result.visualDistanceMeters < actionEnd) {
        result.phase = WorkoutGameFeaturePhase::Action;
    } else if (result.visualDistanceMeters
            < piece->challenge.prepareDistanceMeters) {
        result.phase = WorkoutGameFeaturePhase::Approach;
    } else if (result.visualDistanceMeters
            < piece->challenge.decisionDistanceMeters) {
        result.phase = WorkoutGameFeaturePhase::Measure;
    } else if (result.visualDistanceMeters < actionStart) {
        result.phase = WorkoutGameFeaturePhase::Committed;
    } else if (result.visualDistanceMeters < actionEnd) {
        result.phase = WorkoutGameFeaturePhase::Action;
    } else {
        result.phase = WorkoutGameFeaturePhase::Recovery;
    }

    result.actionId = (std::uint64_t(featureSection + 1) << 32)
            | (std::uint64_t(piece->terrain) + 1u);
    const bool completed = result.outcome
            == WorkoutGameFeatureOutcome::Completed;
    const bool bypass = result.terrain != WorkoutGameTerrainKind::Rollers
            && result.terrain != WorkoutGameTerrainKind::Climb
            && (result.outcome == WorkoutGameFeatureOutcome::Bypassed
                || result.route == WorkoutGameRoute::SafeBypass);
    if (bypass) {
        if (piece->terrain == WorkoutGameTerrainKind::Berm) {
            const WorkoutGameBermGeometryProfile berm =
                    WorkoutGameBermGeometry::profile(piece->difficulty);
            result.lateralOffsetMeters = berm.safeLineLateralMeters(
                    result.visualDistanceMeters
                        - piece->challenge.obstacleDistanceMeters,
                    piece->turnRadians);
        } else if (piece->terrain == WorkoutGameTerrainKind::Roots) {
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
        } else if (piece->terrain == WorkoutGameTerrainKind::Skinny) {
            const WorkoutGameSkinnyGeometryProfile skinny =
                    WorkoutGameSkinnyGeometry::profile(piece->difficulty);
            result.lateralOffsetMeters = skinny.safeLineOffsetMeters(
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
                && !bypass) {
            result.triggerJump = true;
            result.verticalOffsetMeters = jumpHeight(piece->terrain)
                    * jumpArc(actionProgress);
            result.pitchDegrees = 7.0 * std::cos(Pi * actionProgress);
        } else if (result.motion == WorkoutGameFeatureMotion::Drop
                && completed && !bypass) {
            result.pitchDegrees = -14.0 * std::sin(Pi * actionProgress);
            result.verticalOffsetMeters = -0.45 * smoothStep(actionProgress);
        }
    }
    return result;
}
