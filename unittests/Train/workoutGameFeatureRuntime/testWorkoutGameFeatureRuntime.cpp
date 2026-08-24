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

}

class TestWorkoutGameFeatureRuntime : public QObject
{
    Q_OBJECT

private slots:
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
        const WorkoutGameFeatureRuntimeSnapshot actionLayout = runtime.update(
                snapshot(section, progressAtDistance(
                            road, section, takeoff + 0.1),
                         WorkoutGameFeatureOutcome::Completed));

        QCOMPARE(runtime.update(snapshot(
                    section, 0.10, WorkoutGameFeatureOutcome::Active)).phase,
                 WorkoutGameFeaturePhase::Approach);
        QCOMPARE(runtime.update(snapshot(
                    section, 0.68, WorkoutGameFeatureOutcome::Active)).phase,
                 WorkoutGameFeaturePhase::Measure);
        QCOMPARE(runtime.update(snapshot(
                    section, progressAtDistance(
                        road, section,
                        (piece->challenge.decisionDistanceMeters + takeoff)
                            * 0.5),
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

    void rootsAndRockGardenExposeDifferentRoughness()
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
        QVERIFY(roots.vibration > 0.0);
        QVERIFY(rocks.vibration > roots.vibration);
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
            const double distance = takeoff + 0.3
                    * (jumpActionEndDistance(road, section, piece) - takeoff);
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

    void tabletopFlightDurationGrowsWithSpeedButStaysBounded()
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

        QVERIFY(normalFlight.flightDurationSeconds >= 1.5);
        QVERIFY(fastFlight.flightDurationSeconds
                > normalFlight.flightDurationSeconds);
        QVERIFY(fastFlight.flightDurationSeconds <= 5.0);
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

    void dropPitchesTheRiderAndReportsLanding()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        const int section = sectionFor(course, WorkoutGameTerrainKind::Drop);
        const WorkoutGameRoadPiece *piece = challengePieceFor(road, section);
        QVERIFY(piece != nullptr);

        const WorkoutGameFeatureRuntimeSnapshot action = runtime.update(snapshot(
                section, progressAtDistance(
                    road, section,
                    piece->challenge.obstacleDistanceMeters + 1.5),
                WorkoutGameFeatureOutcome::Completed));
        const WorkoutGameFeatureRuntimeSnapshot recovery = runtime.update(snapshot(
                section, progressAtDistance(
                    road, section,
                    piece->challenge.obstacleDistanceMeters + 6.1),
                WorkoutGameFeatureOutcome::Completed));

        QCOMPARE(action.motion, WorkoutGameFeatureMotion::Drop);
        QVERIFY(action.pitchDegrees < -2.0);
        QVERIFY(recovery.landingImpact > 0.0);
    }

    void airbornePolicyDistinguishesRollersFromAnchoredFeatures()
    {
        WorkoutGameFeatureRuntimeSnapshot feature;
        QVERIFY(!WorkoutGameFeatureRuntime::airborneExpected(feature));

        feature.ready = true;
        feature.terrain = WorkoutGameTerrainKind::Rollers;
        feature.phase = WorkoutGameFeaturePhase::Approach;
        QVERIFY(WorkoutGameFeatureRuntime::airborneExpected(feature));

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
        QVERIFY(WorkoutGameFeatureRuntime::airborneExpected(feature));
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameFeatureRuntime)
#include "testWorkoutGameFeatureRuntime.moc"
