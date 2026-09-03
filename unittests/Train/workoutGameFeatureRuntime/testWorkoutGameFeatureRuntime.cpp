/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameFeatureLab.h"
#include "Train/WorkoutGameFeatureGeometry.h"
#include "Train/WorkoutGameFeatureRuntime.h"
#include "Train/WorkoutGameGapJumpGeometry.h"
#include "Train/WorkoutGameBermGeometry.h"
#include "Train/WorkoutGameClimbGeometry.h"
#include "Train/WorkoutGameRootGeometry.h"
#include "Train/WorkoutGameRockGardenGeometry.h"
#include "Train/WorkoutGameRockSlabGeometry.h"
#include "Train/WorkoutGameSkinnyGeometry.h"

#include <QTest>

#include <algorithm>
#include <cmath>

namespace {

WorkoutGameSimulationSnapshot snapshot(
        int section,
        double progress,
        WorkoutGameFeatureOutcome outcome,
        WorkoutGameRoute route = WorkoutGameRoute::MainLine)
{
    WorkoutGameSimulationSnapshot result;
    result.ready = true;
    result.activeSection = section;
    result.sectionProgress = progress;
    result.featureOutcome = outcome;
    result.route = route;
    result.challengeReadiness = outcome == WorkoutGameFeatureOutcome::Completed
            ? 1.0 : 0.45;
    return result;
}

int sectionFor(
        const WorkoutGameCourse &course,
        WorkoutGameTerrainKind terrain)
{
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        if (course.sections[index].terrain == terrain
                && course.sections[index].challengeCount > 0) {
            return int(index);
        }
    }
    return -1;
}

const WorkoutGameRoadPiece *challengePieceFor(
        const WorkoutGameRoadCourse &road,
        int section)
{
    const auto piece = std::find_if(
            road.pieces.begin(), road.pieces.end(),
            [section](const WorkoutGameRoadPiece &candidate) {
                return candidate.sourceSectionIndex == std::size_t(section)
                        && candidate.challenge.enabled;
            });
    return piece != road.pieces.end() ? &*piece : nullptr;
}

double progressAtDistance(
        const WorkoutGameRoadCourse &road,
        int section,
        double distanceMeters)
{
    const WorkoutGameRoadTimelineSection &timeline =
            road.timeline[std::size_t(section)];
    return (distanceMeters - timeline.startDistanceMeters)
            / (timeline.endDistanceMeters - timeline.startDistanceMeters);
}

double jumpTakeoffDistance(const WorkoutGameRoadPiece &piece)
{
    const WorkoutGameFeatureGeometryProfile geometry =
            WorkoutGameFeatureGeometry::profile(
                piece.terrain, piece.difficulty);
    return piece.challenge.obstacleDistanceMeters
            + (piece.terrain == WorkoutGameTerrainKind::Tabletop
                ? geometry.plateauStartMeters : geometry.startMeters);
}

double jumpActionEndDistance(
        const WorkoutGameRoadCourse &road,
        int section,
        const WorkoutGameRoadPiece &piece)
{
    const WorkoutGameFeatureGeometryProfile geometry =
            WorkoutGameFeatureGeometry::profile(
                piece.terrain, piece.difficulty);
    const double takeoff = jumpTakeoffDistance(piece);
    return std::min(
            road.timeline[std::size_t(section)].endDistanceMeters,
            std::max(takeoff + 5.5,
                     piece.challenge.obstacleDistanceMeters
                        + geometry.endMeters + 1.5));
}

WorkoutGameRoadCourse gapJumpRoad()
{
    WorkoutGameRoadCourse road;
    road.ready = true;
    road.seed = 88031u;
    road.totalLengthMeters = 140.0;
    road.visualLengthMeters = road.totalLengthMeters;

    WorkoutGameRoadPiece piece;
    piece.sourceSectionIndex = 0;
    piece.terrain = WorkoutGameTerrainKind::GapJump;
    piece.startDistanceMeters = 0.0;
    piece.lengthMeters = road.totalLengthMeters;
    piece.difficulty = 0.5;
    piece.challenge.enabled = true;
    piece.challenge.prepareDistanceMeters = 35.0;
    piece.challenge.decisionDistanceMeters = 77.0;
    piece.challenge.obstacleDistanceMeters = 80.0;
    const auto profile = WorkoutGameGapJumpGeometry::canonicalProfile();
    piece.gapJump.enabled = true;
    piece.gapJump.prepareDistanceMeters = 35.0;
    piece.gapJump.launchWindowStartDistanceMeters = 70.0;
    piece.gapJump.lockDistanceMeters = 77.0;
    piece.gapJump.splitStartDistanceMeters = 68.0;
    for (std::size_t index = 0; index < profile.lines.size(); ++index) {
        const auto &source = profile.lines[index];
        auto &line = piece.gapJump.lines[index];
        line.id = source.id;
        line.takeoffDistanceMeters = 80.0;
        line.landingDistanceMeters = line.takeoffDistanceMeters
                + source.gapLengthMeters;
        line.lateralMeters = source.lateralMeters;
        line.gapLengthMeters = source.gapLengthMeters;
        line.minimumSpeedMetersPerSecond =
                source.coldThresholdMetersPerSecond;
        line.nominalFlightSeconds = source.nominalFlightSeconds;
        line.lipHeightMeters = source.lipHeightMeters;
        line.landingDropMeters = source.landingDropMeters;
    }
    piece.gapJump.mergeEndDistanceMeters =
            piece.gapJump.lines.back().landingDistanceMeters
            + 6.0 + profile.mergeLengthMeters;
    piece.challenge.bypassStartDistanceMeters =
            piece.gapJump.splitStartDistanceMeters;
    piece.challenge.bypassEndDistanceMeters =
            piece.gapJump.mergeEndDistanceMeters;
    piece.challenge.bypassLateralMeters = profile.bypassLateralMeters;
    road.pieces.push_back(piece);
    road.timeline.push_back({0, 0, 28000, 0.0, road.totalLengthMeters});
    return road;
}

WorkoutGameSimulationSnapshot gapSnapshot(
        double distanceMeters,
        std::int64_t workoutTimeMs,
        double speedMetersPerSecond,
        double actualWatts,
        double targetWatts)
{
    WorkoutGameSimulationSnapshot result = snapshot(
            0, distanceMeters / 140.0,
            WorkoutGameFeatureOutcome::Active);
    result.workoutTimeMs = workoutTimeMs;
    result.speedKph = speedMetersPerSecond * 3.6;
    result.challengeMeasurementActive = true;
    result.challengeMetrics.averageActualWatts = actualWatts;
    result.challengeMetrics.averageTargetWatts = targetWatts;
    result.challengeMetrics.averageSpeedKph = result.speedKph;
    return result;
}

WorkoutGameFeatureRuntimeSnapshot rideGapTo(
        WorkoutGameFeatureRuntime &runtime,
        double &distanceMeters,
        double endDistanceMeters,
        std::int64_t &workoutTimeMs,
        double speedMetersPerSecond,
        double actualWatts,
        double targetWatts,
        int stepMilliseconds = 50)
{
    WorkoutGameFeatureRuntimeSnapshot result;
    while (distanceMeters < endDistanceMeters) {
        workoutTimeMs += stepMilliseconds;
        distanceMeters = std::min(
                endDistanceMeters,
                distanceMeters + speedMetersPerSecond
                    * stepMilliseconds / 1000.0);
        result = runtime.update(gapSnapshot(
                distanceMeters, workoutTimeMs, speedMetersPerSecond,
                actualWatts, targetWatts), actualWatts, targetWatts);
    }
    return result;
}

WorkoutGameCourse climbWithRunoutCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 7411u;
    course.durationMs = 30000;
    WorkoutGameSection climb;
    climb.feature = WorkoutGameFeature::Climb;
    climb.terrain = WorkoutGameTerrainKind::Climb;
    climb.durationMs = course.durationMs;
    climb.lengthMeters = 90.0;
    climb.targetWatts = 220.0;
    climb.gradePercent = 8.0;
    climb.difficulty = 0.65;
    climb.challengeCount = 1;
    WorkoutGameSection runout;
    runout.feature = WorkoutGameFeature::RecoveryDescent;
    runout.terrain = WorkoutGameTerrainKind::SmoothTrail;
    runout.startMs = climb.durationMs;
    runout.durationMs = 12000;
    runout.lengthMeters = 36.0;
    runout.targetWatts = 100.0;
    runout.gradePercent = -3.0;
    course.sections = {climb, runout};
    course.durationMs += runout.durationMs;
    return course;
}

}

class TestWorkoutGameFeatureRuntime : public QObject
{
    Q_OBJECT

private slots:
    void climbUsesTheCanonicalMainLineEffortWindow()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const int section = sectionFor(course, WorkoutGameTerrainKind::Climb);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);
        const auto profile = WorkoutGameClimbGeometry::profile(piece->difficulty);

        const WorkoutGameFeatureRuntimeSnapshot weak = runtime.update(snapshot(
                section, 0.98, WorkoutGameFeatureOutcome::Bypassed,
                WorkoutGameRoute::SafeBypass));
        QCOMPARE(weak.route, WorkoutGameRoute::MainLine);
        QCOMPARE(weak.motion, WorkoutGameFeatureMotion::Climb);
        QCOMPARE(weak.phase, WorkoutGameFeaturePhase::Action);
        QCOMPARE(weak.lateralOffsetMeters, 0.0);
        QCOMPARE(weak.actionStartDistanceMeters,
                 piece->challenge.obstacleDistanceMeters
                    + profile.activeStartMeters);
        QCOMPARE(weak.actionEndDistanceMeters,
                 piece->challenge.obstacleDistanceMeters
                    + profile.endMeters);
        QVERIFY(!WorkoutGameFeatureRuntime::airborneExpected(weak));
        for (const WorkoutGameClimbStep &step : profile.steps) {
            const double distance = piece->challenge.obstacleDistanceMeters
                    + step.forwardMeters;
            const auto atStep = runtime.update(snapshot(
                    section, progressAtDistance(road, section, distance),
                    WorkoutGameFeatureOutcome::Active));
            QCOMPARE(atStep.phase, WorkoutGameFeaturePhase::Action);
            QCOMPARE(atStep.motion, WorkoutGameFeatureMotion::Climb);
            QCOMPARE(atStep.route, WorkoutGameRoute::MainLine);
        }
    }

    void climbResultPersistsForSixMetersOfAnUnchallengedRunout()
    {
        const WorkoutGameCourse course = climbWithRunoutCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        WorkoutGameSimulationSnapshot runout = snapshot(
                1, 3.0 / 36.0, WorkoutGameFeatureOutcome::None);
        runout.previousFeatureSection = 0;
        runout.previousFeatureOutcome =
                WorkoutGameFeatureOutcome::Bypassed;
        runout.previousFeatureReadiness = 0.0;
        const WorkoutGameFeatureRuntimeSnapshot result =
                runtime.update(runout);
        QVERIFY(result.ready);
        QCOMPARE(result.sourceSectionIndex, 0);
        QCOMPARE(result.terrain, WorkoutGameTerrainKind::Climb);
        QCOMPARE(result.phase, WorkoutGameFeaturePhase::Recovery);
        QCOMPARE(result.outcome, WorkoutGameFeatureOutcome::Bypassed);
        QCOMPARE(result.route, WorkoutGameRoute::MainLine);

        runout.sectionProgress = 7.0 / 36.0;
        const WorkoutGameFeatureRuntimeSnapshot expired =
                runtime.update(runout);
        QCOMPARE(expired.sourceSectionIndex, 1);
        QCOMPARE(expired.terrain, WorkoutGameTerrainKind::SmoothTrail);
        QCOMPARE(expired.phase, WorkoutGameFeaturePhase::None);
    }

    void skinnyUsesCanonicalBalanceWindowWithoutAChickenLine()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const int section = sectionFor(course, WorkoutGameTerrainKind::Skinny);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);
        const auto skinny = WorkoutGameSkinnyGeometry::profile(
                piece->difficulty);
        const double center = piece->challenge.obstacleDistanceMeters;
        const auto at = [&](double distance, WorkoutGameRoute route) {
            return runtime.update(snapshot(
                    section, progressAtDistance(road, section, distance),
                    route == WorkoutGameRoute::MainLine
                        ? WorkoutGameFeatureOutcome::Completed
                        : WorkoutGameFeatureOutcome::Bypassed,
                    route));
        };

        const auto main = at(center, WorkoutGameRoute::MainLine);
        QCOMPARE(main.motion, WorkoutGameFeatureMotion::Balance);
        QCOMPARE(main.actionStartDistanceMeters,
                 center + skinny.activeStartMeters);
        QCOMPARE(main.actionEndDistanceMeters,
                 center + skinny.activeEndMeters);
        QCOMPARE(main.vibration, 0.0);
        QCOMPARE(main.verticalOffsetMeters, 0.0);
        QCOMPARE(main.pitchDegrees, 0.0);
        QVERIFY(!main.triggerJump);
        const auto missed = at(center, WorkoutGameRoute::SafeBypass);
        QCOMPARE(missed.outcome, WorkoutGameFeatureOutcome::Bypassed);
        QCOMPARE(missed.lateralOffsetMeters, 0.0);
    }

    void invalidCoursesFailClosed()
    {
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(!runtime.configure(WorkoutGameRoadCourse()));
        QVERIFY(!runtime.update(WorkoutGameSimulationSnapshot()).ready);
    }

    void phaseProgressesThroughTheAnchoredGate()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const int section = sectionFor(course, WorkoutGameTerrainKind::LogOver);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);
        const double takeoff = jumpTakeoffDistance(*piece);
        const double sectionStart =
                road.timeline[std::size_t(section)].startDistanceMeters;
        const double prepare = piece->challenge.prepareDistanceMeters;
        const double decision = piece->challenge.decisionDistanceMeters;
        QVERIFY(sectionStart < prepare);
        QVERIFY(prepare < decision);
        QVERIFY(decision < takeoff);
        const WorkoutGameFeatureRuntimeSnapshot actionLayout = runtime.update(
                snapshot(section, progressAtDistance(
                            road, section, takeoff + 0.1),
                         WorkoutGameFeatureOutcome::Completed));

        QCOMPARE(runtime.update(snapshot(
                    section, progressAtDistance(
                        road, section, (sectionStart + prepare) * 0.5),
                    WorkoutGameFeatureOutcome::Active)).phase,
                 WorkoutGameFeaturePhase::Approach);
        QCOMPARE(runtime.update(snapshot(
                    section, progressAtDistance(
                        road, section, (prepare + decision) * 0.5),
                    WorkoutGameFeatureOutcome::Active)).phase,
                 WorkoutGameFeaturePhase::Measure);
        QCOMPARE(runtime.update(snapshot(
                    section, progressAtDistance(
                        road, section,
                        (decision + takeoff) * 0.5),
                    WorkoutGameFeatureOutcome::Completed)).phase,
                 WorkoutGameFeaturePhase::Committed);
        QCOMPARE(runtime.update(snapshot(
                    section, progressAtDistance(
                        road, section, takeoff + 0.1),
                    WorkoutGameFeatureOutcome::Completed)).phase,
                 WorkoutGameFeaturePhase::Action);
        QCOMPARE(runtime.update(snapshot(
                    section, progressAtDistance(
                        road, section,
                        actionLayout.actionEndDistanceMeters + 0.1),
                    WorkoutGameFeatureOutcome::Completed)).phase,
                 WorkoutGameFeaturePhase::Recovery);
    }

    void completedLogJumpsAtTheObstacle()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const int section = sectionFor(course, WorkoutGameTerrainKind::LogOver);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);
        const double takeoff = jumpTakeoffDistance(*piece);
        const double actionDistance = takeoff + 0.3
                * (jumpActionEndDistance(road, section, *piece) - takeoff);

        const WorkoutGameFeatureRuntimeSnapshot result = runtime.update(
                snapshot(section, progressAtDistance(
                            road, section, actionDistance),
                         WorkoutGameFeatureOutcome::Completed));

        QCOMPARE(result.motion, WorkoutGameFeatureMotion::Jump);
        QVERIFY(result.triggerJump);
        QVERIFY(result.verticalOffsetMeters > 0.2);
        QCOMPARE(result.lateralOffsetMeters, 0.0);
        QVERIFY(result.actionId != 0u);
    }

    void airbornePolicyStartsAtThePhysicalTakeoff()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        QVERIFY(road.ready);

        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const WorkoutGameTerrainKind terrains[] = {
            WorkoutGameTerrainKind::LogOver,
            WorkoutGameTerrainKind::Tabletop
        };
        for (WorkoutGameTerrainKind terrain : terrains) {
            const int section = sectionFor(course, terrain);
            QVERIFY(section >= 0);
            const auto piece = std::find_if(
                    road.pieces.begin(), road.pieces.end(),
                    [section](const WorkoutGameRoadPiece &candidate) {
                        return candidate.sourceSectionIndex
                                    == std::size_t(section)
                                && candidate.challenge.enabled;
                    });
            QVERIFY(piece != road.pieces.end());
            const WorkoutGameFeatureGeometryProfile geometry =
                    WorkoutGameFeatureGeometry::profile(
                        terrain, piece->difficulty);
            QVERIFY(geometry.ready);
            const double takeoffOffset = terrain
                    == WorkoutGameTerrainKind::Tabletop
                ? geometry.plateauStartMeters : geometry.startMeters;
            const WorkoutGameRoadTimelineSection &timeline =
                    road.timeline[std::size_t(section)];
            const double sectionLength = timeline.endDistanceMeters
                    - timeline.startDistanceMeters;
            const double takeoffProgress = (
                    piece->challenge.obstacleDistanceMeters + takeoffOffset
                    - timeline.startDistanceMeters) / sectionLength;

            const WorkoutGameFeatureRuntimeSnapshot before = runtime.update(
                    snapshot(section, takeoffProgress - 0.0001,
                             WorkoutGameFeatureOutcome::Completed));
            const WorkoutGameFeatureRuntimeSnapshot takeoff = runtime.update(
                    snapshot(section, takeoffProgress + 0.0001,
                             WorkoutGameFeatureOutcome::Completed));
            QCOMPARE(before.phase, WorkoutGameFeaturePhase::Committed);
            QCOMPARE(takeoff.phase, WorkoutGameFeaturePhase::Action);
            QVERIFY(takeoff.triggerJump);
            QVERIFY(!WorkoutGameFeatureRuntime::airborneExpected(before));
            QVERIFY(WorkoutGameFeatureRuntime::airborneExpected(takeoff));
        }
    }

    void missedLogUsesVisibleBypassWithoutJumping()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const int section = sectionFor(course, WorkoutGameTerrainKind::LogOver);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);
        const double branchMiddle =
                (piece->challenge.bypassStartDistanceMeters
                 + piece->challenge.bypassEndDistanceMeters) * 0.5;

        const WorkoutGameFeatureRuntimeSnapshot result = runtime.update(
                snapshot(section,
                         progressAtDistance(road, section, branchMiddle),
                         WorkoutGameFeatureOutcome::Bypassed,
                         WorkoutGameRoute::SafeBypass));

        QVERIFY(!result.triggerJump);
        QCOMPARE(result.verticalOffsetMeters, 0.0);
        QVERIFY(std::abs(result.lateralOffsetMeters) > 1.0);

        const WorkoutGameFeatureRuntimeSnapshot recovered = runtime.update(
                snapshot(section, progressAtDistance(
                            road, section,
                            piece->challenge.bypassEndDistanceMeters + 0.1),
                         WorkoutGameFeatureOutcome::Bypassed,
                         WorkoutGameRoute::SafeBypass));
        QVERIFY(std::abs(recovered.lateralOffsetMeters) < 1e-9);
    }

    void technicalSurfacesUsePhysicsSuspensionInsteadOfRuntimeVibration()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(
                WorkoutGameRoadCourseBuilder::build(course, 200.0)));
        const auto completed = WorkoutGameFeatureOutcome::Completed;
        const WorkoutGameFeatureRuntimeSnapshot roots = runtime.update(snapshot(
                sectionFor(course, WorkoutGameTerrainKind::Roots),
                0.94, completed));
        const WorkoutGameFeatureRuntimeSnapshot rocks = runtime.update(snapshot(
                sectionFor(course, WorkoutGameTerrainKind::RockGarden),
                0.94, completed));

        QCOMPARE(roots.motion, WorkoutGameFeatureMotion::Absorb);
        QCOMPARE(rocks.motion, WorkoutGameFeatureMotion::Absorb);
        QCOMPARE(roots.vibration, 0.0);
        QCOMPARE(rocks.vibration, 0.0);
    }

    void rootsSafeLineStaysOnTheWidenedTileAndRejoinsTheSockets()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const int section = sectionFor(course, WorkoutGameTerrainKind::Roots);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);
        const WorkoutGameRootGeometryProfile roots =
                WorkoutGameRootGeometry::profile(piece->difficulty);
        const double center = piece->challenge.obstacleDistanceMeters;

        const auto bypassAt = [&](double distance) {
            return runtime.update(snapshot(
                    section, progressAtDistance(road, section, distance),
                    WorkoutGameFeatureOutcome::Bypassed,
                    WorkoutGameRoute::SafeBypass));
        };
        QCOMPARE(bypassAt(center + roots.startMeters).lateralOffsetMeters, 0.0);
        const WorkoutGameFeatureRuntimeSnapshot middle = bypassAt(center);
        QCOMPARE(std::abs(middle.lateralOffsetMeters),
                 roots.safeLineLateralMeters);
        QVERIFY(std::abs(middle.lateralOffsetMeters)
                < roots.activeHalfWidthMeters);
        QCOMPARE(bypassAt(center + roots.endMeters).lateralOffsetMeters, 0.0);
        QVERIFY(!middle.triggerJump);
        QCOMPARE(middle.actionStartDistanceMeters,
                 center + roots.activeStartMeters);
        QCOMPARE(middle.actionEndDistanceMeters,
                 center + roots.activeEndMeters);
    }

    void rockGardenSafeLineStaysOnTheWidenedTileAndRejoinsTheSockets()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const int section = sectionFor(
                course, WorkoutGameTerrainKind::RockGarden);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);
        const WorkoutGameRockGardenGeometryProfile rocks =
                WorkoutGameRockGardenGeometry::profile(piece->difficulty);
        const double center = piece->challenge.obstacleDistanceMeters;

        const auto bypassAt = [&](double distance) {
            return runtime.update(snapshot(
                    section, progressAtDistance(road, section, distance),
                    WorkoutGameFeatureOutcome::Bypassed,
                    WorkoutGameRoute::SafeBypass));
        };
        QCOMPARE(bypassAt(center + rocks.startMeters).lateralOffsetMeters, 0.0);
        const WorkoutGameFeatureRuntimeSnapshot middle = bypassAt(center);
        QCOMPARE(middle.lateralOffsetMeters,
                 rocks.safeLineLateralMeters);
        QVERIFY(middle.lateralOffsetMeters < rocks.activeHalfWidthMeters);
        QCOMPARE(bypassAt(center + rocks.endMeters).lateralOffsetMeters, 0.0);
        QVERIFY(!middle.triggerJump);
        QCOMPARE(middle.actionStartDistanceMeters,
                 center + rocks.activeStartMeters);
        QCOMPARE(middle.actionEndDistanceMeters,
                 center + rocks.activeEndMeters);
    }

    void rockSlabUsesCanonicalAbsorbWindowAndSmoothSameTreadSafeLine()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const int section = sectionFor(
                course, WorkoutGameTerrainKind::RockSlab);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);
        const WorkoutGameRockSlabGeometryProfile slab =
                WorkoutGameRockSlabGeometry::profile(piece->difficulty);
        const double center = piece->challenge.obstacleDistanceMeters;
        const auto at = [&](double distance, WorkoutGameRoute route) {
            return runtime.update(snapshot(
                    section, progressAtDistance(road, section, distance),
                    route == WorkoutGameRoute::MainLine
                        ? WorkoutGameFeatureOutcome::Completed
                        : WorkoutGameFeatureOutcome::Bypassed,
                    route));
        };

        const WorkoutGameFeatureRuntimeSnapshot main = at(
                center, WorkoutGameRoute::MainLine);
        QCOMPARE(main.motion, WorkoutGameFeatureMotion::Absorb);
        QCOMPARE(main.actionStartDistanceMeters,
                 center + slab.activeStartMeters);
        QCOMPARE(main.actionEndDistanceMeters,
                 center + slab.activeEndMeters);
        QCOMPARE(main.vibration, 0.0);
        QCOMPARE(main.verticalOffsetMeters, 0.0);
        QCOMPARE(main.pitchDegrees, 0.0);
        QVERIFY(!main.triggerJump);

        QCOMPARE(at(center + slab.startMeters,
                    WorkoutGameRoute::SafeBypass).lateralOffsetMeters, 0.0);
        QCOMPARE(at(center, WorkoutGameRoute::SafeBypass).lateralOffsetMeters,
                 slab.safeLineLateralMeters);
        QCOMPARE(at(center + slab.endMeters,
                    WorkoutGameRoute::SafeBypass).lateralOffsetMeters, 0.0);

        double previousLateral = 0.0;
        constexpr double FrameTravelMeters = 5.0 / 60.0;
        for (double local = slab.startMeters;
             local <= slab.endMeters; local += FrameTravelMeters) {
            const double lateral = at(
                    center + local,
                    WorkoutGameRoute::SafeBypass).lateralOffsetMeters;
            QVERIFY(std::abs(lateral - previousLateral) < 0.05);
            previousLateral = lateral;
        }
    }

    void tabletopHasMoreAirThanTheLog()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const auto completed = WorkoutGameFeatureOutcome::Completed;
        const int logSection = sectionFor(
                course, WorkoutGameTerrainKind::LogOver);
        const int tabletopSection = sectionFor(
                course, WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadPiece *logPiece =
                challengePieceFor(road, logSection);
        const WorkoutGameRoadPiece *tabletopPiece =
                challengePieceFor(road, tabletopSection);
        QVERIFY(logPiece != nullptr);
        QVERIFY(tabletopPiece != nullptr);
        const auto actionProgress = [&](int section,
                                        const WorkoutGameRoadPiece &piece) {
            const double takeoff = jumpTakeoffDistance(piece);
            const WorkoutGameFeatureRuntimeSnapshot layout = runtime.update(
                    snapshot(section, progressAtDistance(
                                road, section, takeoff + 0.1), completed));
            const double distance = layout.actionStartDistanceMeters + 0.3
                    * (layout.actionEndDistanceMeters
                       - layout.actionStartDistanceMeters);
            return progressAtDistance(road, section, distance);
        };
        const WorkoutGameFeatureRuntimeSnapshot log = runtime.update(snapshot(
                logSection, actionProgress(logSection, *logPiece), completed));
        const WorkoutGameFeatureRuntimeSnapshot tabletop = runtime.update(snapshot(
                tabletopSection,
                actionProgress(tabletopSection, *tabletopPiece), completed));

        QCOMPARE(tabletop.motion, WorkoutGameFeatureMotion::Jump);
        QVERIFY(tabletop.verticalOffsetMeters > log.verticalOffsetMeters);
    }

    void bunnyHopHasShorterLowerAirThanTheLog()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const auto completed = WorkoutGameFeatureOutcome::Completed;
        const int bunnySection = sectionFor(
                course, WorkoutGameTerrainKind::BunnyHop);
        const int logSection = sectionFor(
                course, WorkoutGameTerrainKind::LogOver);
        const WorkoutGameRoadPiece *bunnyPiece =
                challengePieceFor(road, bunnySection);
        const WorkoutGameRoadPiece *logPiece =
                challengePieceFor(road, logSection);
        QVERIFY(bunnyPiece != nullptr);
        QVERIFY(logPiece != nullptr);
        const auto atApex = [&](int section,
                                const WorkoutGameRoadPiece &piece) {
            const double takeoff = jumpTakeoffDistance(piece);
            const WorkoutGameFeatureRuntimeSnapshot layout = runtime.update(
                    snapshot(section, progressAtDistance(
                                road, section, takeoff + 0.01), completed));
            const double distance = layout.actionStartDistanceMeters + 0.30
                    * (layout.actionEndDistanceMeters
                       - layout.actionStartDistanceMeters);
            return runtime.update(snapshot(
                    section, progressAtDistance(road, section, distance),
                    completed));
        };
        const WorkoutGameFeatureRuntimeSnapshot bunny =
                atApex(bunnySection, *bunnyPiece);
        const WorkoutGameFeatureRuntimeSnapshot log =
                atApex(logSection, *logPiece);

        QVERIFY(bunny.triggerJump);
        QVERIFY(log.triggerJump);
        QVERIFY(bunny.verticalOffsetMeters < log.verticalOffsetMeters);
        QVERIFY(bunny.flightDurationSeconds < log.flightDurationSeconds);
        QVERIFY(bunny.flightDurationSeconds <= 1.1);
    }

    void jumpArcHasVisibleTakeoffApexAndLanding()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(
                WorkoutGameRoadCourseBuilder::build(course, 200.0)));
        const int section = sectionFor(course, WorkoutGameTerrainKind::Tabletop);
        const auto completed = WorkoutGameFeatureOutcome::Completed;
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);
        const WorkoutGameFeatureRuntimeSnapshot actionLayout = runtime.update(
                snapshot(section, progressAtDistance(
                            road, section, jumpTakeoffDistance(*piece) + 0.1),
                         completed));
        const double actionStart = actionLayout.actionStartDistanceMeters;
        const double actionSpan = actionLayout.actionEndDistanceMeters
                - actionStart;

        const WorkoutGameFeatureRuntimeSnapshot takeoff = runtime.update(
                snapshot(section, progressAtDistance(
                            road, section, actionStart + actionSpan * 0.08),
                         completed));
        const WorkoutGameFeatureRuntimeSnapshot apex = runtime.update(
                snapshot(section, progressAtDistance(
                            road, section, actionStart + actionSpan * 0.30),
                         completed));
        const WorkoutGameFeatureRuntimeSnapshot landing = runtime.update(
                snapshot(section, progressAtDistance(
                            road, section, actionStart + actionSpan * 0.75),
                         completed));

        QCOMPARE(takeoff.phase, WorkoutGameFeaturePhase::Action);
        QCOMPARE(apex.phase, WorkoutGameFeaturePhase::Action);
        QCOMPARE(landing.phase, WorkoutGameFeaturePhase::Action);
        QVERIFY(takeoff.verticalOffsetMeters > 0.05);
        QVERIFY(apex.verticalOffsetMeters > takeoff.verticalOffsetMeters);
        QVERIFY(apex.verticalOffsetMeters > landing.verticalOffsetMeters);
        QVERIFY(landing.verticalOffsetMeters > 0.05);
    }

    void tabletopFlightDurationUsesAuthoritativeTimelineSpeed()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const int section = sectionFor(course, WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);
        const double takeoff = jumpTakeoffDistance(*piece);
        const double progress = progressAtDistance(
                road, section, takeoff + 0.5);

        WorkoutGameSimulationSnapshot normal = snapshot(
                section, progress, WorkoutGameFeatureOutcome::Completed);
        normal.speedKph = 20.0;
        WorkoutGameSimulationSnapshot fast = normal;
        fast.speedKph = 42.0;
        const WorkoutGameFeatureRuntimeSnapshot normalFlight =
                runtime.update(normal);
        const WorkoutGameFeatureRuntimeSnapshot fastFlight =
                runtime.update(fast);

        QVERIFY2(normalFlight.flightDurationSeconds >= 0.45,
                 qPrintable(QStringLiteral(
                     "normal tabletop flight lasted only %1 seconds")
                     .arg(normalFlight.flightDurationSeconds)));
        QCOMPARE(fastFlight.flightDurationSeconds,
                 normalFlight.flightDurationSeconds);
        const WorkoutGameRoadTimelineSection &timeline =
                road.timeline[std::size_t(section)];
        const double timelineSpeed =
                (timeline.endDistanceMeters - timeline.startDistanceMeters)
                    * 1000.0 / double(timeline.durationMs);
        const double expected = WorkoutGameTabletopGeometry::profile(
                piece->difficulty).flightDurationSeconds(timelineSpeed);
        QVERIFY(std::abs(normalFlight.flightDurationSeconds - expected)
                < 1e-9);
        QVERIFY(normalFlight.flightDurationSeconds <= 2.0);
        QVERIFY(fastFlight.flightDurationSeconds <= 2.0);
    }

    void bypassLineEasesInAndOutWithoutLateralJumps()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(
                WorkoutGameRoadCourseBuilder::build(course, 200.0)));
        const int section = sectionFor(course, WorkoutGameTerrainKind::LogOver);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);
        const auto bypassed = WorkoutGameFeatureOutcome::Bypassed;
        const auto route = WorkoutGameRoute::SafeBypass;

        const double atBranch = runtime.update(
                snapshot(section, progressAtDistance(
                            road, section,
                            piece->challenge.bypassStartDistanceMeters),
                         bypassed, route)).lateralOffsetMeters;
        const double justAfterBranch = runtime.update(
                snapshot(section, progressAtDistance(
                            road, section,
                            piece->challenge.bypassStartDistanceMeters + 0.05),
                         bypassed, route)).lateralOffsetMeters;
        const double nearReturn = runtime.update(
                snapshot(section, progressAtDistance(
                            road, section,
                            piece->challenge.bypassEndDistanceMeters - 0.05),
                         bypassed, route)).lateralOffsetMeters;
        const double afterReturn = runtime.update(
                snapshot(section, progressAtDistance(
                            road, section,
                            piece->challenge.bypassEndDistanceMeters),
                         bypassed, route)).lateralOffsetMeters;

        QVERIFY(std::abs(atBranch) < 1e-9);
        QVERIFY(std::abs(justAfterBranch - atBranch) < 0.01);
        QVERIFY(std::abs(afterReturn - nearReturn) < 0.01);
        QVERIFY(std::abs(afterReturn) < 1e-9);
    }

    void ambientBermUsesEffortToSelectAContinuousBankLine()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const auto sectionIt = std::find_if(
                course.sections.begin(), course.sections.end(),
                [](const WorkoutGameSection &candidate) {
                    return candidate.terrain == WorkoutGameTerrainKind::Berm;
                });
        QVERIFY(sectionIt != course.sections.end());
        const int section = int(std::distance(course.sections.begin(), sectionIt));
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [section](const WorkoutGameRoadPiece &candidate) {
                    return candidate.sourceSectionIndex == std::size_t(section)
                            && candidate.terrain == WorkoutGameTerrainKind::Berm;
                });
        QVERIFY(piece != road.pieces.end());
        QVERIFY(!piece->challenge.enabled);
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(piece->difficulty);
        const auto lineAt = [&](double local, double watts) {
            return runtime.update(
                    snapshot(section,
                             progressAtDistance(
                                road, section,
                                piece->geometryAnchorDistanceMeters + local),
                             WorkoutGameFeatureOutcome::Completed),
                    watts, 180.0);
        };

        const auto target = lineAt(0.0, 180.0);
        const auto above = lineAt(0.0, 270.0);
        const auto below = lineAt(0.0, 90.0);
        QCOMPARE(target.phase, WorkoutGameFeaturePhase::None);
        QCOMPARE(target.outcome, WorkoutGameFeatureOutcome::None);
        QCOMPARE(target.actionId, std::uint64_t(0));
        QVERIFY(std::abs(target.lateralOffsetMeters) < 1e-9);
        QVERIFY(above.bermLineBias > 0.99);
        QVERIFY(below.bermLineBias < -0.99);
        QVERIFY(above.lateralOffsetMeters * piece->turnRadians < 0.0);
        QVERIFY(below.lateralOffsetMeters * piece->turnRadians > 0.0);
        QVERIFY(std::abs(above.lateralOffsetMeters) <= 0.55);
        QCOMPARE(lineAt(profile.startMeters, 270.0).lateralOffsetMeters, 0.0);
        QCOMPARE(lineAt(profile.endMeters, 90.0).lateralOffsetMeters, 0.0);
    }

    void dropPoseDoesNotSynthesizeASecondLandingImpact()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const int section = sectionFor(course, WorkoutGameTerrainKind::Drop);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);

        const WorkoutGameFeatureRuntimeSnapshot layout = runtime.update(
                snapshot(section, 0.99,
                         WorkoutGameFeatureOutcome::Completed));
        const double actionDistance = layout.actionStartDistanceMeters
                + 0.5 * (layout.actionEndDistanceMeters
                         - layout.actionStartDistanceMeters);
        const WorkoutGameFeatureRuntimeSnapshot action = runtime.update(snapshot(
                section, progressAtDistance(
                    road, section, actionDistance),
                WorkoutGameFeatureOutcome::Completed));
        const WorkoutGameFeatureRuntimeSnapshot recovery = runtime.update(snapshot(
                section, progressAtDistance(
                    road, section,
                    layout.actionEndDistanceMeters + 0.1),
                WorkoutGameFeatureOutcome::Completed));

        QCOMPARE(action.motion, WorkoutGameFeatureMotion::Drop);
        QCOMPARE(action.phase, WorkoutGameFeaturePhase::Action);
        QVERIFY2(action.pitchDegrees < -2.0,
                 qPrintable(QStringLiteral(
                     "drop action pitch was %1 degrees")
                     .arg(action.pitchDegrees)));
        QCOMPARE(recovery.landingImpact, 0.0);
    }

    void airbornePolicyKeepsSurfaceFollowingRollersGrounded()
    {
        WorkoutGameFeatureRuntimeSnapshot feature;
        QVERIFY(!WorkoutGameFeatureRuntime::airborneExpected(feature));

        feature.ready = true;
        feature.terrain = WorkoutGameTerrainKind::Rollers;
        feature.phase = WorkoutGameFeaturePhase::Approach;
        QVERIFY(!WorkoutGameFeatureRuntime::airborneExpected(feature));

        feature.terrain = WorkoutGameTerrainKind::LogOver;
        feature.motion = WorkoutGameFeatureMotion::Jump;
        feature.phase = WorkoutGameFeaturePhase::Measure;
        QVERIFY(!WorkoutGameFeatureRuntime::airborneExpected(feature));

        feature.phase = WorkoutGameFeaturePhase::Action;
        QVERIFY(WorkoutGameFeatureRuntime::airborneExpected(feature));

        feature.terrain = WorkoutGameTerrainKind::SmoothTrail;
        feature.motion = WorkoutGameFeatureMotion::None;
        QVERIFY(!WorkoutGameFeatureRuntime::airborneExpected(feature));
    }

    void rollerSectionsWithoutChallengesRetainTheirTerrainPolicy()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.durationMs = 12000;
        course.seed = 7u;
        WorkoutGameSection section;
        section.terrain = WorkoutGameTerrainKind::Rollers;
        section.durationMs = course.durationMs;
        section.targetWatts = 180.0;
        section.difficulty = 0.5;
        section.challengeCount = 0;
        course.sections.push_back(section);

        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(
                WorkoutGameRoadCourseBuilder::build(course, 200.0)));
        const WorkoutGameFeatureRuntimeSnapshot feature = runtime.update(
                snapshot(0, 0.5, WorkoutGameFeatureOutcome::Active));

        QVERIFY(feature.ready);
        QCOMPARE(feature.terrain, WorkoutGameTerrainKind::Rollers);
        QCOMPARE(feature.phase, WorkoutGameFeaturePhase::None);
        QVERIFY(!WorkoutGameFeatureRuntime::airborneExpected(feature));
    }

    void failedRollerFlowDoesNotCreateABypassMotion()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const int section = sectionFor(course, WorkoutGameTerrainKind::Rollers);

        const WorkoutGameFeatureRuntimeSnapshot feature = runtime.update(
                snapshot(section, 0.8,
                         WorkoutGameFeatureOutcome::Bypassed,
                         WorkoutGameRoute::SafeBypass));

        QVERIFY(feature.ready);
        QCOMPARE(feature.terrain, WorkoutGameTerrainKind::Rollers);
        QCOMPARE(feature.route, WorkoutGameRoute::MainLine);
        QCOMPARE(feature.lateralOffsetMeters, 0.0);
        QVERIFY(!WorkoutGameFeatureRuntime::airborneExpected(feature));
    }

    void gapJumpPreviewStaysUnlockedUntilThreeMetersBeforeTheLip()
    {
        const WorkoutGameRoadCourse road = gapJumpRoad();
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        runtime.update(gapSnapshot(30.0, 0, 7.0, 230.0, 220.0),
                       230.0, 220.0);
        const auto measure = runtime.update(gapSnapshot(
                67.5, 50, 7.0, 230.0, 220.0), 230.0, 220.0);
        QCOMPARE(measure.phase, WorkoutGameFeaturePhase::Measure);
        QCOMPARE(measure.prepareDistanceMeters,
                 road.pieces.front().gapJump.prepareDistanceMeters);
        QCOMPARE(measure.launchWindowStartDistanceMeters,
                 road.pieces.front().gapJump.launchWindowStartDistanceMeters);
        QCOMPARE(measure.decisionDistanceMeters,
                 road.pieces.front().gapJump.lockDistanceMeters);
        QCOMPARE(measure.provisionalGapLine, WorkoutGameGapJumpLine::Long);
        QVERIFY(!measure.gapLineLocked);
        QVERIFY(!measure.launchWindowActive);
        QCOMPARE(measure.lateralOffsetMeters, 0.0);
    }

    void gapJumpUsesBest500msSpeedOnlyInsideTheLaunchWindow()
    {
        const WorkoutGameRoadCourse road = gapJumpRoad();
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        double distance = 67.5;
        std::int64_t time = 0;
        const auto preview = runtime.update(gapSnapshot(
                distance, time, 7.0, 230.0, 220.0), 230.0, 220.0);
        const std::uint64_t actionId = preview.actionId;
        rideGapTo(runtime, distance, 74.0, time, 7.0, 230.0, 220.0);
        const auto launch = runtime.update(gapSnapshot(
                distance, time, 4.0, 230.0, 220.0), 230.0, 220.0);
        QVERIFY(launch.launchWindowActive);
        QVERIFY(launch.launchSpeedReady);
        QVERIFY(launch.launchPowerReady);
        QCOMPARE(launch.provisionalGapLine, WorkoutGameGapJumpLine::Long);
        QCOMPARE(launch.launchBestSpeedMetersPerSecond, 7.0);

        const auto locked = rideGapTo(
                runtime, distance, 77.0, time, 4.0, 230.0, 220.0);
        QCOMPARE(locked.phase, WorkoutGameFeaturePhase::Committed);
        QVERIFY(locked.gapLineLocked);
        QCOMPARE(locked.lockedGapLine, WorkoutGameGapJumpLine::Long);
        QCOMPARE(locked.route, WorkoutGameRoute::MainLine);
        QCOMPARE(locked.actionId, actionId);

        const auto afterTelemetryChange = runtime.update(
                gapSnapshot(78.0, time + 50, 2.0, 0.0, 220.0), 0.0, 220.0);
        QCOMPARE(afterTelemetryChange.lockedGapLine,
                 WorkoutGameGapJumpLine::Long);
        QCOMPARE(afterTelemetryChange.actionId, locked.actionId);
    }

    void gapJumpIgnoresSpeedBeforeTheTenMeterLaunchWindow()
    {
        const WorkoutGameRoadCourse road = gapJumpRoad();
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        double distance = 69.0;
        std::int64_t time = 0;
        runtime.update(gapSnapshot(
                distance, time, 12.0, 230.0, 220.0), 230.0, 220.0);
        rideGapTo(runtime, distance, 69.95, time,
                  12.0, 230.0, 220.0);
        const auto locked = rideGapTo(
                runtime, distance, 77.0, time, 5.5, 230.0, 220.0);

        QVERIFY(locked.gapLineLocked);
        QCOMPARE(locked.lockedGapLine, WorkoutGameGapJumpLine::Medium);
        QVERIFY(locked.launchBestSpeedMetersPerSecond < 5.6);
    }

    void gapJumpSingleFastTickCannotSelectTheLongLine()
    {
        const WorkoutGameRoadCourse road = gapJumpRoad();
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        double distance = 70.0;
        std::int64_t time = 0;
        runtime.update(gapSnapshot(
                distance, time, 5.9, 230.0, 220.0), 230.0, 220.0);
        for (int sample = 0; sample < 9; ++sample) {
            rideGapTo(runtime, distance, distance + 0.295, time,
                      5.9, 230.0, 220.0);
        }
        rideGapTo(runtime, distance, distance + 0.5, time,
                  10.0, 230.0, 220.0);
        const auto locked = rideGapTo(
                runtime, distance, 77.0, time, 5.9, 230.0, 220.0);
        QVERIFY(locked.launchBestSpeedMetersPerSecond < 6.6);
        QVERIFY(locked.gapLineLocked);
        QVERIFY(locked.lockedGapLine != WorkoutGameGapJumpLine::Long);
    }

    void gapJumpPowerDipResetsTheContinuousHold()
    {
        const WorkoutGameRoadCourse road = gapJumpRoad();
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        double distance = 70.0;
        std::int64_t time = 0;
        runtime.update(gapSnapshot(
                distance, time, 7.0, 230.0, 220.0), 230.0, 220.0);
        rideGapTo(runtime, distance, 74.2, time, 7.0, 230.0, 220.0);
        QVERIFY(runtime.update(gapSnapshot(
                    distance, time, 7.0, 230.0, 220.0), 230.0, 220.0)
                .launchPowerReady);
        rideGapTo(runtime, distance, distance + 0.35, time,
                  7.0, 219.0, 220.0);
        const auto locked = rideGapTo(
                runtime, distance, 77.0, time, 7.0, 230.0, 220.0);
        QVERIFY(locked.gapLineLocked);
        QCOMPARE(locked.lockedGapLine, WorkoutGameGapJumpLine::None);
        QCOMPARE(locked.route, WorkoutGameRoute::SafeBypass);
        QCOMPARE(locked.outcome, WorkoutGameFeatureOutcome::Bypassed);
        QVERIFY(!locked.launchPowerReady);
    }

    void gapJumpLaneChangesAndLockRemainContinuous()
    {
        const WorkoutGameRoadCourse road = gapJumpRoad();
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const auto &gate = road.pieces.front().gapJump;
        double distance = 67.5;
        std::int64_t time = 0;
        auto frame = runtime.update(gapSnapshot(
                distance, time, 7.0, 230.0, 220.0), 230.0, 220.0);
        const std::uint64_t actionId = frame.actionId;
        double previous = frame.lateralOffsetMeters;
        double maximumStep = 0.0;
        while (distance < gate.mergeEndDistanceMeters) {
            frame = rideGapTo(
                    runtime, distance,
                    std::min(gate.mergeEndDistanceMeters,
                             distance + 0.35),
                    time, 7.0, 230.0, 220.0);
            maximumStep = std::max(
                    maximumStep,
                    std::abs(frame.lateralOffsetMeters - previous));
            previous = frame.lateralOffsetMeters;
            QCOMPARE(frame.actionId, actionId);
        }
        QVERIFY(maximumStep <= 0.15 + 1e-9);
        QCOMPARE(frame.lockedGapLine, WorkoutGameGapJumpLine::Long);
        QVERIFY(std::abs(frame.lateralOffsetMeters) < 0.20);
    }

    void gapJumpLateLongPromotionDowngradesToAReachableLine()
    {
        const WorkoutGameRoadCourse road = gapJumpRoad();
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        double distance = 67.5;
        std::int64_t time = 0;
        auto frame = runtime.update(gapSnapshot(
                distance, time, 5.5, 230.0, 220.0), 230.0, 220.0);
        rideGapTo(runtime, distance, 73.5, time, 5.5, 230.0, 220.0);
        double previousLateral = frame.lateralOffsetMeters;
        double maximumStep = 0.0;
        while (distance < 77.0) {
            frame = rideGapTo(runtime, distance,
                              std::min(77.0, distance + 0.35),
                              time, 7.0, 230.0, 220.0);
            maximumStep = std::max(
                    maximumStep,
                    std::abs(frame.lateralOffsetMeters - previousLateral));
            previousLateral = frame.lateralOffsetMeters;
        }

        QVERIFY(frame.gapLineLocked);
        QCOMPARE(frame.provisionalGapLine, WorkoutGameGapJumpLine::Medium);
        QCOMPARE(frame.lockedGapLine, WorkoutGameGapJumpLine::Medium);
        QVERIFY(!frame.gapLineReachable);
        QVERIFY(maximumStep <= 0.15 + 1e-9);

        const auto afterLock = runtime.update(gapSnapshot(
                78.0, time + 50, 9.0, 300.0, 220.0), 300.0, 220.0);
        QCOMPARE(afterLock.lockedGapLine, WorkoutGameGapJumpLine::Medium);
        QVERIFY(!afterLock.gapLineReachable);
    }

    void gapJumpDistanceJumpAcrossTheWindowFailsClosed()
    {
        const WorkoutGameRoadCourse road = gapJumpRoad();
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        runtime.update(gapSnapshot(
                69.0, 0, 7.0, 230.0, 220.0), 230.0, 220.0);
        const auto skipped = runtime.update(gapSnapshot(
                78.0, 1000, 7.0, 230.0, 220.0), 230.0, 220.0);
        QVERIFY(skipped.gapLineLocked);
        QCOMPARE(skipped.lockedGapLine, WorkoutGameGapJumpLine::None);
        QVERIFY(!skipped.launchSpeedReady);
    }

    void gapJumpTimingDiscontinuityCannotBackfillTheLaunchWindow()
    {
        const WorkoutGameRoadCourse road = gapJumpRoad();
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        const auto first = runtime.update(gapSnapshot(
                70.0, 0, 7.0, 230.0, 220.0), 230.0, 220.0);
        QVERIFY(!first.launchSpeedReady);
        QCOMPARE(first.launchPowerHoldMilliseconds, 0);
        const auto discontinuity = runtime.update(gapSnapshot(
                70.35, 1000, 7.0, 230.0, 220.0), 230.0, 220.0);
        QVERIFY(!discontinuity.launchSpeedReady);
        QCOMPARE(discontinuity.launchPowerHoldMilliseconds, 0);

        double distance = 70.35;
        std::int64_t time = 1000;
        rideGapTo(runtime, distance, 73.15, time,
                  7.0, 230.0, 220.0, 50);
        const auto interrupted = runtime.update(gapSnapshot(
                distance + 0.35, time + 250,
                7.0, 230.0, 220.0), 230.0, 220.0);
        QVERIFY(!interrupted.launchSpeedReady);
        QCOMPARE(interrupted.launchPowerHoldMilliseconds, 0);
    }

    void gapJumpUsesSelectedGapAndBoundedAuthoredFlight()
    {
        const WorkoutGameRoadCourse road = gapJumpRoad();
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        double distance = 67.5;
        std::int64_t time = 0;
        runtime.update(gapSnapshot(
                distance, time, 5.5, 230.0, 220.0), 230.0, 220.0);
        rideGapTo(runtime, distance, 77.0, time, 5.5, 230.0, 220.0);

        const auto profile = WorkoutGameGapJumpGeometry::canonicalProfile();
        const WorkoutGameRoadGapJumpLine &medium =
                road.pieces.front().gapJump.lines[1];
        const auto apex = runtime.update(gapSnapshot(
                medium.takeoffDistanceMeters
                    + medium.gapLengthMeters * 0.35,
                time + 50, 5.5, 230.0, 220.0), 230.0, 220.0);
        QCOMPARE(apex.phase, WorkoutGameFeaturePhase::Action);
        QCOMPARE(apex.lockedGapLine, WorkoutGameGapJumpLine::Medium);
        QCOMPARE(apex.physicalTakeoffDistanceMeters,
                 medium.takeoffDistanceMeters);
        QCOMPARE(apex.actionEndDistanceMeters,
                 medium.landingDistanceMeters);
        QCOMPARE(apex.selectedGapLengthMeters, medium.gapLengthMeters);
        QCOMPARE(apex.flightDurationSeconds,
                 medium.gapLengthMeters / 5.5);
        QVERIFY(apex.flightDurationSeconds <= profile.maximumFlightSeconds);
        QVERIFY(apex.flightDurationSeconds <= 2.0);
        QVERIFY(apex.triggerJump);
        QVERIFY(apex.verticalOffsetMeters > 0.0);
        QVERIFY(WorkoutGameFeatureRuntime::airborneExpected(apex));

        const auto landed = runtime.update(gapSnapshot(
                apex.actionEndDistanceMeters, time + 100,
                5.5, 230.0, 220.0),
                230.0, 220.0);
        QCOMPARE(landed.phase, WorkoutGameFeaturePhase::Recovery);
        QVERIFY(!landed.triggerJump);
        QCOMPARE(landed.verticalOffsetMeters, 0.0);
        QVERIFY(!WorkoutGameFeatureRuntime::airborneExpected(landed));
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameFeatureRuntime)
#include "testWorkoutGameFeatureRuntime.moc"
