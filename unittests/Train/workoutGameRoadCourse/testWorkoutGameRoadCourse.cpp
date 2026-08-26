/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameRoadCourse.h"
#include "Train/WorkoutGameBermGeometry.h"
#include "Train/WorkoutGameClimbGeometry.h"
#include "Train/WorkoutGameRoadProjection.h"
#include "Train/WorkoutGameFeatureGeometry.h"
#include "Train/WorkoutGameHorizon.h"
#include "Train/WorkoutGameRootGeometry.h"
#include "Train/WorkoutGameRockGardenGeometry.h"
#include "Train/WorkoutGameRockSlabGeometry.h"
#include "Train/WorkoutGameSkinnyGeometry.h"
#include "Train/WorkoutGameTabletopGeometry.h"
#include "Train/WorkoutGameTrailBranch.h"

#include <QTest>

#include <cmath>
#include <limits>

namespace {

WorkoutGameCourse sampleCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 123u;
    course.durationMs = 30000;
    WorkoutGameSection climb;
    climb.feature = WorkoutGameFeature::Climb;
    climb.terrain = WorkoutGameTerrainKind::Climb;
    climb.durationMs = 15000;
    climb.targetWatts = 220.0;
    climb.gradePercent = 8.0;
    climb.difficulty = 0.7;
    climb.challengeCount = 1;
    climb.visualVariant = 2u;
    WorkoutGameSection log = climb;
    log.feature = WorkoutGameFeature::SprintJump;
    log.terrain = WorkoutGameTerrainKind::LogOver;
    log.startMs = climb.durationMs;
    log.durationMs = 15000;
    log.targetWatts = 260.0;
    log.gradePercent = 0.0;
    log.difficulty = 0.5;
    log.visualVariant = 3u;
    course.sections = {climb, log};
    return course;
}

WorkoutGameCourse crestCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 912u;
    WorkoutGameSection climb;
    climb.feature = WorkoutGameFeature::Climb;
    climb.terrain = WorkoutGameTerrainKind::Climb;
    climb.durationMs = 10000;
    climb.targetWatts = 220.0;
    climb.gradePercent = 15.0;
    WorkoutGameSection descent = climb;
    descent.startMs = 10000;
    descent.gradePercent = -15.0;
    WorkoutGameSection finish = climb;
    finish.startMs = 20000;
    finish.gradePercent = 0.0;
    course.durationMs = 30000;
    course.sections = {climb, descent, finish};
    return course;
}

bool near(double left, double right, double tolerance = 1e-7)
{
    return std::abs(left - right) <= tolerance;
}

}

class TestWorkoutGameRoadCourse : public QObject
{
    Q_OBJECT

private slots:
    void tabletopGeometryIsCanonicalAndDeterministic()
    {
        const auto profile = WorkoutGameTabletopGeometry::profile(0.7);
        const auto repeated = WorkoutGameTabletopGeometry::profile(0.7);
        const auto invalid = WorkoutGameTabletopGeometry::profile(
                std::numeric_limits<double>::quiet_NaN());
        QVERIFY(profile.ready);
        QCOMPARE(profile.startMeters, repeated.startMeters);
        QCOMPARE(profile.endMeters, repeated.endMeters);
        QVERIFY(std::abs(profile.heightMeters - 0.654) < 1e-12);
        QVERIFY(profile.lipMeters - profile.startMeters >= 2.0);
        QVERIFY(std::abs(profile.deckEndMeters - profile.lipMeters - 2.49)
                < 1e-12);
        QVERIFY(profile.endMeters - profile.deckEndMeters
                > profile.lipMeters - profile.startMeters);
        QVERIFY(profile.endMeters - profile.startMeters < 8.0);
        QCOMPARE(profile.surfaceOffsetMeters(profile.startMeters), 0.0);
        QCOMPARE(profile.surfaceOffsetMeters(profile.lipMeters),
                 profile.heightMeters);
        QCOMPARE(profile.surfaceOffsetMeters(profile.deckEndMeters),
                 profile.heightMeters);
        QCOMPARE(profile.surfaceOffsetMeters(profile.endMeters), 0.0);
        QCOMPARE(invalid.heightMeters, 0.50);
        for (const double difficulty : {0.0, 0.35, 0.7, 1.0}) {
            const auto canonical =
                    WorkoutGameTabletopGeometry::profile(difficulty);
            const auto generic = WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::Tabletop, difficulty);
            for (int sample = 0; sample <= 80; ++sample) {
                const double local = canonical.startMeters
                        + (canonical.endMeters - canonical.startMeters)
                            * double(sample) / 80.0;
                QVERIFY(std::abs(generic.surfaceOffset(local)
                                 - canonical.surfaceOffsetMeters(local))
                        < 1e-12);
            }
            for (const double speed : {3.0, 5.0, 7.0, 8.0}) {
                const double landing =
                        canonical.plannedLandingLocalMeters(speed);
                QVERIFY(landing > canonical.deckEndMeters);
                QVERIFY(landing < canonical.endMeters);
                QVERIFY(canonical.flightDurationSeconds(speed) <= 2.0);
            }
            QVERIFY(!canonical.supportsJumpAtForwardSpeed(2.99));
            QVERIFY(canonical.supportsJumpAtForwardSpeed(3.0));
            QVERIFY(canonical.supportsJumpAtForwardSpeed(8.0));
            QVERIFY(!canonical.supportsJumpAtForwardSpeed(8.01));
        }
    }

    void tabletopSafeLineStartsAtDecisionAndClearsTheFeature()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 981u;
        course.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::Tabletop;
        section.durationMs = course.durationMs;
        section.targetWatts = 260.0;
        section.difficulty = 0.7;
        section.challengeCount = 1;
        course.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const auto profile = WorkoutGameTabletopGeometry::profile(
                piece->difficulty);
        const WorkoutGameRoadSample entry =
                WorkoutGameRoadCourseBuilder::sample(
                    road, piece->challenge.obstacleDistanceMeters
                        + profile.startMeters);
        const WorkoutGameRoadSample exit =
                WorkoutGameRoadCourseBuilder::sample(
                    road, piece->challenge.obstacleDistanceMeters
                        + profile.endMeters);
        QVERIFY(std::abs(entry.center.halfWidthMeters
                         - profile.socketHalfWidthMeters) < 1e-12);
        QVERIFY(std::abs(exit.center.halfWidthMeters
                         - profile.socketHalfWidthMeters) < 1e-12);
        QCOMPARE(piece->challenge.bypassStartDistanceMeters,
                 piece->challenge.decisionDistanceMeters);
        QVERIFY(piece->challenge.prepareDistanceMeters
                <= piece->challenge.decisionDistanceMeters
                    - profile.splitLeadMeters + 1e-9);
        QVERIFY(piece->challenge.bypassEndDistanceMeters
                - piece->challenge.bypassStartDistanceMeters
                    >= profile.minimumBypassLengthMeters - 1e-9);
        QVERIFY(piece->challenge.bypassEndDistanceMeters
                >= piece->challenge.obstacleDistanceMeters
                    + profile.endMeters
                    + profile.bypassExitRunoutMeters - 1e-9);
        const double atObstacle = std::abs(
                WorkoutGameTrailBranch::lateralAt(
                    piece->challenge.obstacleDistanceMeters,
                    piece->challenge.bypassStartDistanceMeters,
                    piece->challenge.bypassEndDistanceMeters,
                    piece->challenge.bypassLateralMeters));
        QVERIFY(atObstacle > 1.5);
    }

    void shortExplicitTabletopPreservesDistanceWithoutClippingAChallenge()
    {
        for (const double length : {5.0, 20.0, 28.0, 35.9}) {
            WorkoutGameCourse course;
            course.status = WorkoutGameCourseStatus::Ready;
            course.seed = 1181u;
            course.durationMs = 5000;
            WorkoutGameSection section;
            section.feature = WorkoutGameFeature::SprintJump;
            section.terrain = WorkoutGameTerrainKind::Tabletop;
            section.durationMs = course.durationMs;
            section.lengthMeters = length;
            section.targetWatts = 260.0;
            section.difficulty = 1.0;
            section.challengeCount = 1;
            course.sections = {section};

            const WorkoutGameRoadCourse road =
                    WorkoutGameRoadCourseBuilder::build(course, 200.0);
            QVERIFY(road.ready);
            QCOMPARE(road.totalLengthMeters, length);
            QVERIFY(std::none_of(
                    road.pieces.begin(), road.pieces.end(),
                    [](const WorkoutGameRoadPiece &piece) {
                        return piece.challenge.enabled;
                    }));
            for (double distance = 0.0; distance <= length;
                 distance += 0.25) {
                const WorkoutGameRoadSample sample =
                        WorkoutGameRoadCourseBuilder::sample(road, distance);
                QVERIFY(sample.ready);
                QCOMPARE(sample.surfaceOffsetMeters, 0.0);
                QVERIFY(std::isfinite(sample.center.elevationMeters));
            }
        }
    }

    void minimumExplicitTabletopProvidesTheCompleteBypass()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 1182u;
        course.durationMs = 12000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::Tabletop;
        section.durationMs = course.durationMs;
        section.lengthMeters = 36.0;
        section.targetWatts = 260.0;
        section.difficulty = 1.0;
        section.challengeCount = 1;
        course.sections = {section};

        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        QCOMPARE(road.totalLengthMeters, 36.0);
        QCOMPARE(piece->challenge.bypassStartDistanceMeters, 8.0);
        QCOMPARE(piece->challenge.bypassEndDistanceMeters, 36.0);
        QCOMPARE(piece->challenge.bypassEndDistanceMeters
                    - piece->challenge.bypassStartDistanceMeters,
                 28.0);
    }

    void climbProfileIsDeterministicAndHasExactTrailSockets()
    {
        const auto profile = WorkoutGameClimbGeometry::profile(0.65);
        const auto repeated = WorkoutGameClimbGeometry::profile(0.65);
        const auto invalid = WorkoutGameClimbGeometry::profile(
                std::numeric_limits<double>::quiet_NaN());

        QVERIFY(profile.ready);
        QVERIFY(invalid.ready);
        QCOMPARE(profile.startMeters, repeated.startMeters);
        QCOMPARE(profile.endMeters, repeated.endMeters);
        QCOMPARE(profile.steps, repeated.steps);
        QCOMPARE(profile.socketHalfWidthMeters, 0.68);
        QVERIFY(profile.startMeters < profile.activeStartMeters);
        QVERIFY(profile.activeStartMeters < profile.crestStartMeters);
        QVERIFY(profile.crestStartMeters < profile.endMeters);
        QCOMPARE(profile.endMeters, 0.0);
        QCOMPARE(profile.minimumLengthMeters,
                 profile.endMeters - profile.startMeters);
        QCOMPARE(profile.surfaceOffsetMeters(profile.startMeters), 0.0);
        QCOMPARE(profile.surfaceOffsetMeters(profile.endMeters), 0.0);
        for (const WorkoutGameClimbStep &step : profile.steps) {
            QVERIFY(std::isfinite(step.forwardMeters));
            QVERIFY(std::isfinite(step.lateralMeters));
            QVERIFY(step.halfLengthMeters > 0.0);
            QVERIFY(step.halfWidthMeters > 0.0);
            QVERIFY(step.heightMeters > 0.0);
            QVERIFY(step.forwardMeters >= profile.activeStartMeters);
            QVERIFY(step.forwardMeters < profile.crestStartMeters);
            QCOMPARE(profile.surfaceOffsetMeters(step.forwardMeters),
                     step.heightMeters);
        }
    }

    void climbStaysOnOneTreadAndUsesABoundedContinuousCrest()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 9021u;
        course.durationMs = 60000;
        WorkoutGameSection climb;
        climb.feature = WorkoutGameFeature::Climb;
        climb.terrain = WorkoutGameTerrainKind::Climb;
        climb.durationMs = 40000;
        climb.lengthMeters = 120.0;
        climb.targetWatts = 240.0;
        climb.gradePercent = 9.0;
        climb.difficulty = 0.65;
        climb.challengeCount = 1;
        WorkoutGameSection recovery = climb;
        recovery.feature = WorkoutGameFeature::Trail;
        recovery.terrain = WorkoutGameTerrainKind::SmoothTrail;
        recovery.startMs = climb.durationMs;
        recovery.durationMs = 20000;
        recovery.lengthMeters = 60.0;
        recovery.targetWatts = 100.0;
        recovery.gradePercent = 0.0;
        recovery.challengeCount = 0;
        course.sections = {climb, recovery};

        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        QVERIFY(road.ready);
        const auto profile = WorkoutGameClimbGeometry::profile(0.65);
        const double crest = road.timeline[0].endDistanceMeters;
        const auto challenge = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.terrain == WorkoutGameTerrainKind::Climb
                            && piece.challenge.enabled;
                });
        QVERIFY(challenge != road.pieces.end());
        QCOMPARE(challenge->challenge.obstacleDistanceMeters, crest);
        QCOMPARE(challenge->challenge.bypassStartDistanceMeters,
                 challenge->challenge.bypassEndDistanceMeters);
        QCOMPARE(challenge->challenge.bypassLateralMeters, 0.0);

        const double sustainedGrade = profile.sustainedGradePercent(
                120.0, 9.0, 0.0, 0.0,
                profile.entryTransitionMeters,
                profile.crestTransitionMeters);
        const auto beforeTransition = WorkoutGameRoadCourseBuilder::sample(
                road, crest - profile.crestTransitionMeters - 0.05);
        const auto beforeSocket = WorkoutGameRoadCourseBuilder::sample(
                road, crest - 0.001);
        const auto atSocket = WorkoutGameRoadCourseBuilder::sample(road, crest);
        const auto afterSocket = WorkoutGameRoadCourseBuilder::sample(
                road, crest + 0.001);
        QVERIFY(beforeTransition.ready);
        QVERIFY(beforeSocket.ready);
        QVERIFY(atSocket.ready);
        QVERIFY(afterSocket.ready);
        QVERIFY(std::abs(beforeTransition.baseGradePercent
                         - sustainedGrade) < 0.1);
        QVERIFY(std::abs(beforeSocket.baseGradePercent) < 0.1);
        QVERIFY(std::abs(atSocket.baseGradePercent) < 1e-9);
        QVERIFY(std::abs(atSocket.center.gradePercent) < 0.1);
        QVERIFY(std::abs(afterSocket.center.gradePercent) < 0.1);
        QVERIFY(std::abs(beforeSocket.center.elevationMeters
                         - atSocket.center.elevationMeters) < 0.01);
        QVERIFY(std::abs(afterSocket.center.elevationMeters
                         - atSocket.center.elevationMeters) < 0.01);
        const auto start = WorkoutGameRoadCourseBuilder::sample(road, 0.0);
        QVERIFY(start.ready);
        QVERIFY(std::abs(atSocket.baseElevationMeters
                         - start.baseElevationMeters - 10.8) < 1e-9);

        double previousGrade = sustainedGrade + 0.01;
        for (double distance = crest - profile.crestTransitionMeters;
             distance <= crest; distance += 0.05) {
            const auto sample = WorkoutGameRoadCourseBuilder::sample(
                    road, distance);
            QVERIFY(sample.ready);
            QVERIFY(sample.baseGradePercent >= -0.01);
            QVERIFY(sample.baseGradePercent <= sustainedGrade + 0.01);
            QVERIFY(sample.baseGradePercent <= previousGrade + 0.01);
            previousGrade = sample.baseGradePercent;
        }
        for (const WorkoutGameClimbStep &step : profile.steps) {
            const auto sample = WorkoutGameRoadCourseBuilder::sample(
                    road, crest + step.forwardMeters);
            QVERIFY(sample.ready);
            QCOMPARE(sample.surfaceOffsetMeters, step.heightMeters);
        }
    }

    void climbNormalizesShortTilesAndBoundsHugePieceCounts()
    {
        const auto profile = WorkoutGameClimbGeometry::profile(0.5);
        for (const double requestedLength : {1.0, 5.0, 10.0, 14.0, 24.0}) {
            WorkoutGameCourse course;
            course.status = WorkoutGameCourseStatus::Ready;
            course.durationMs = 10000;
            WorkoutGameSection section;
            section.feature = WorkoutGameFeature::Climb;
            section.terrain = WorkoutGameTerrainKind::Climb;
            section.durationMs = course.durationMs;
            section.lengthMeters = requestedLength;
            section.targetWatts = 220.0;
            section.gradePercent = 8.0;
            section.difficulty = 0.5;
            section.challengeCount = 1;
            course.sections = {section};
            const WorkoutGameRoadCourse road =
                    WorkoutGameRoadCourseBuilder::build(course, 200.0);
            QVERIFY(road.ready);
            QVERIFY(road.totalLengthMeters >= profile.minimumLengthMeters);
            const auto piece = std::find_if(
                    road.pieces.begin(), road.pieces.end(),
                    [](const WorkoutGameRoadPiece &candidate) {
                        return candidate.challenge.enabled;
                    });
            QVERIFY(piece != road.pieces.end());
            QVERIFY(piece->challenge.obstacleDistanceMeters
                    + profile.startMeters >= -1e-9);
        }

        WorkoutGameCourse huge;
        huge.status = WorkoutGameCourseStatus::Ready;
        huge.durationMs = 10000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::Climb;
        section.terrain = WorkoutGameTerrainKind::Climb;
        section.durationMs = huge.durationMs;
        section.lengthMeters = 1.0e12;
        section.targetWatts = 220.0;
        section.gradePercent = 8.0;
        section.challengeCount = 1;
        huge.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(huge, 200.0);
        QVERIFY(road.ready);
        QVERIFY(road.pieces.size() <= 24u);
    }

    void skinnyProfileHasRaisedDeckSocketsAndDeterministicBalance()
    {
        const auto profile = WorkoutGameSkinnyGeometry::profile(0.65);
        const auto repeated = WorkoutGameSkinnyGeometry::profile(0.65);
        const auto easiest = WorkoutGameSkinnyGeometry::profile(0.0);
        const auto hardest = WorkoutGameSkinnyGeometry::profile(1.0);
        const auto invalid = WorkoutGameSkinnyGeometry::profile(
                std::numeric_limits<double>::quiet_NaN());
        QVERIFY(profile.ready);
        QCOMPARE(profile.startMeters, -9.0);
        QCOMPARE(profile.activeStartMeters, -7.0);
        QCOMPARE(profile.safeLineStartMeters, -5.0);
        QCOMPARE(profile.deckStartMeters, -3.5);
        QCOMPARE(profile.deckEndMeters, 3.5);
        QCOMPARE(profile.safeLineEndMeters, 5.0);
        QCOMPARE(profile.activeEndMeters, 7.0);
        QCOMPARE(profile.endMeters, 9.0);
        QCOMPARE(profile.socketHalfWidthMeters, 0.68);
        QCOMPARE(profile.activeHalfWidthMeters, 1.55);
        QCOMPARE(profile.safeLineLateralMeters, 1.05);
        QCOMPARE(profile.safeLineSurfaceLiftMeters, 0.012);
        QCOMPARE(invalid.deckHeightMeters, easiest.deckHeightMeters);
        QCOMPARE(repeated.deckHeightMeters, profile.deckHeightMeters);
        QVERIFY(easiest.deckHeightMeters < hardest.deckHeightMeters);
        QVERIFY(easiest.deckHalfWidthMeters > hardest.deckHalfWidthMeters);
        QVERIFY(profile.deckHalfWidthMeters >= 0.25);
        QVERIFY(profile.deckHalfWidthMeters <= 0.31);
        QVERIFY(std::atan(hardest.deckHeightMeters
                         / (hardest.deckStartMeters
                            - hardest.activeStartMeters))
                    * 180.0 / 3.14159265358979323846 <= 6.0);

        QCOMPARE(profile.deckSurfaceOffsetMeters(profile.startMeters), 0.0);
        QCOMPARE(profile.deckSurfaceOffsetMeters(profile.activeStartMeters), 0.0);
        QCOMPARE(profile.deckSurfaceOffsetMeters(0.0),
                 profile.deckHeightMeters);
        QCOMPARE(profile.deckSurfaceOffsetMeters(profile.activeEndMeters), 0.0);
        QCOMPARE(profile.deckSurfaceOffsetMeters(profile.endMeters), 0.0);
        QCOMPARE(profile.surfaceOffsetMeters(
                    0.0, profile.safeLineLateralMeters), 0.0);
        QCOMPARE(profile.safeLineOffsetMeters(profile.startMeters), 0.0);
        QCOMPARE(profile.safeLineOffsetMeters(0.0),
                 profile.safeLineLateralMeters);
        QCOMPARE(profile.safeLineOffsetMeters(profile.endMeters), 0.0);
        QCOMPARE(profile.halfWidthMeters(profile.startMeters),
                 profile.socketHalfWidthMeters);
        QCOMPARE(profile.halfWidthMeters(0.0),
                 profile.activeHalfWidthMeters);

        double maximumBalance = 0.0;
        for (double local = profile.deckStartMeters;
             local <= profile.deckEndMeters; local += 0.02) {
            maximumBalance = std::max(
                    maximumBalance,
                    std::abs(profile.balanceRollDegrees(local)));
            QCOMPARE(profile.balanceRollDegrees(local),
                     repeated.balanceRollDegrees(local));
        }
        QVERIFY(maximumBalance >= 1.0);
        QVERIFY(maximumBalance <= 2.0);
        QCOMPARE(profile.safeLineOffsetMeters(
                    std::numeric_limits<double>::quiet_NaN()), 0.0);
        QCOMPARE(profile.halfWidthMeters(
                    std::numeric_limits<double>::quiet_NaN()),
                 profile.socketHalfWidthMeters);
        QCOMPARE(profile.balanceRollDegrees(
                    std::numeric_limits<double>::quiet_NaN()), 0.0);
        QCOMPARE(profile.balanceRollDegrees(profile.startMeters), 0.0);
        QCOMPARE(profile.balanceRollDegrees(profile.endMeters), 0.0);
    }

    void skinnyRoadDecidesBeforeTheCompleteSocketedTile()
    {
        WorkoutGameCourse source;
        source.status = WorkoutGameCourseStatus::Ready;
        source.seed = 1201u;
        source.durationMs = 40000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::Trail;
        section.terrain = WorkoutGameTerrainKind::Skinny;
        section.durationMs = source.durationMs;
        section.lengthMeters = 220.0;
        section.targetWatts = 175.0;
        section.difficulty = 0.65;
        section.challengeCount = 1;
        source.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(source, 200.0);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const auto profile = WorkoutGameSkinnyGeometry::profile(
                piece->difficulty);
        const double center = piece->challenge.obstacleDistanceMeters;
        QVERIFY(center + profile.startMeters
                >= piece->startDistanceMeters - 1e-9);
        QVERIFY(center + profile.endMeters
                <= piece->startDistanceMeters + piece->lengthMeters + 1e-9);
        QCOMPARE(piece->challenge.decisionDistanceMeters,
                 center + profile.startMeters);
        QVERIFY(piece->challenge.prepareDistanceMeters
                < piece->challenge.decisionDistanceMeters);
        QVERIFY(piece->challenge.decisionDistanceMeters
                - piece->challenge.prepareDistanceMeters <= 6.0 + 1e-9);
        QCOMPARE(piece->challenge.bypassStartDistanceMeters,
                 piece->challenge.bypassEndDistanceMeters);
        QCOMPARE(piece->challenge.bypassLateralMeters, 0.0);
        QVERIFY(WorkoutGameRoadCourseBuilder::sample(
                    road, center + profile.startMeters)
                .renderableTrailSurface);
        QVERIFY(!WorkoutGameRoadCourseBuilder::sample(road, center)
                .renderableTrailSurface);
        QVERIFY(WorkoutGameRoadCourseBuilder::sample(
                    road, center + profile.endMeters)
                .renderableTrailSurface);
        for (double local = profile.startMeters;
             local <= profile.endMeters; local += 0.05) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(road, center + local);
            QVERIFY(sample.ready);
            QVERIFY(std::abs(sample.surfaceOffsetMeters
                        - profile.surfaceOffsetMeters(local, 0.0)) < 1e-9);
            QVERIFY(std::abs(sample.center.halfWidthMeters
                        - profile.halfWidthMeters(local)) < 1e-9);
        }
    }

    void rockSlabProfileHasAsymmetricMassAndSameTreadSafeLine()
    {
        const WorkoutGameRockSlabGeometryProfile profile =
                WorkoutGameRockSlabGeometry::profile(0.65);
        const WorkoutGameRockSlabGeometryProfile repeated =
                WorkoutGameRockSlabGeometry::profile(0.65);
        const WorkoutGameRockSlabGeometryProfile easiest =
                WorkoutGameRockSlabGeometry::profile(0.0);
        const WorkoutGameRockSlabGeometryProfile hardest =
                WorkoutGameRockSlabGeometry::profile(1.0);
        const WorkoutGameRockSlabGeometryProfile invalid =
                WorkoutGameRockSlabGeometry::profile(
                    std::numeric_limits<double>::quiet_NaN());
        QVERIFY(!WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::RockSlab, 0.65).ready);
        QVERIFY(profile.ready);
        QCOMPARE(profile.startMeters, -7.0);
        QCOMPARE(profile.activeStartMeters, -3.8);
        QCOMPARE(profile.crestMeters, 0.25);
        QCOMPARE(profile.activeEndMeters, 3.6);
        QCOMPARE(profile.endMeters, 7.0);
        QCOMPARE(profile.socketHalfWidthMeters, 0.68);
        QCOMPARE(profile.activeHalfWidthMeters, 1.42);
        QCOMPARE(profile.safeLineLateralMeters, 1.05);
        QVERIFY(profile.heightMeters >= 0.78);
        QVERIFY(profile.heightMeters <= 0.90);
        QVERIFY(profile.sideDepthMeters >= 0.12);
        QVERIFY(profile.sideDepthMeters <= 0.30);
        QCOMPARE(repeated.heightMeters, profile.heightMeters);
        QCOMPARE(repeated.sideDepthMeters, profile.sideDepthMeters);
        QCOMPARE(invalid.heightMeters, easiest.heightMeters);
        QCOMPARE(invalid.sideDepthMeters, easiest.sideDepthMeters);
        QVERIFY(easiest.heightMeters < hardest.heightMeters);
        QVERIFY(easiest.sideDepthMeters < hardest.sideDepthMeters);

        QCOMPARE(profile.surfaceOffsetMeters(profile.startMeters, 0.0), 0.0);
        QCOMPARE(profile.surfaceOffsetMeters(profile.endMeters, 0.0), 0.0);
        QCOMPARE(profile.safeLineOffsetMeters(profile.startMeters), 0.0);
        QVERIFY(profile.safeLineOffsetMeters(-5.5) > 0.0);
        QCOMPARE(profile.safeLineOffsetMeters(0.0),
                 profile.safeLineLateralMeters);
        QVERIFY(profile.safeLineOffsetMeters(5.5) > 0.0);
        QCOMPARE(profile.safeLineOffsetMeters(profile.endMeters), 0.0);
        QCOMPARE(profile.halfWidthMeters(profile.startMeters),
                 profile.socketHalfWidthMeters);
        QCOMPARE(profile.halfWidthMeters(0.0),
                 profile.activeHalfWidthMeters);

        const double approachHeight =
                profile.surfaceOffsetMeters(-2.0, 0.0);
        const double crestHeight =
                profile.surfaceOffsetMeters(0.0, 0.0);
        const double rolloverHeight =
                profile.surfaceOffsetMeters(2.0, 0.0);
        QVERIFY(approachHeight > 0.0);
        QVERIFY(approachHeight < crestHeight);
        QVERIFY(rolloverHeight > 0.0);
        QVERIFY(rolloverHeight < crestHeight);
        QVERIFY(std::abs(profile.slabCenterLateralMeters(-2.0)
                         - profile.slabCenterLateralMeters(2.0)) > 0.03);
        QVERIFY(profile.slabHalfWidthMeters(0.0) >= 0.72);

        double maximumMainHeight = 0.0;
        double maximumSafeHeight = 0.0;
        for (double local = profile.activeStartMeters;
             local <= profile.activeEndMeters; local += 0.01) {
            maximumMainHeight = std::max(
                    maximumMainHeight,
                    profile.surfaceOffsetMeters(local, 0.0));
            maximumSafeHeight = std::max(
                    maximumSafeHeight,
                    profile.surfaceOffsetMeters(
                        local, profile.safeLineLateralMeters));
        }
        QVERIFY(maximumMainHeight >= profile.heightMeters * 0.95);
        QVERIFY(maximumSafeHeight <= 0.025);
        QVERIFY(maximumSafeHeight <= maximumMainHeight * 0.04);
    }

    void rockSlabRoadDecidesBeforeTheCanonicalSameTreadTile()
    {
        WorkoutGameCourse source;
        source.status = WorkoutGameCourseStatus::Ready;
        source.seed = 1147u;
        source.durationMs = 40000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::Trail;
        section.terrain = WorkoutGameTerrainKind::RockSlab;
        section.durationMs = source.durationMs;
        section.lengthMeters = 220.0;
        section.targetWatts = 210.0;
        section.difficulty = 0.65;
        section.challengeCount = 1;
        source.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(source, 200.0);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameRockSlabGeometryProfile profile =
                WorkoutGameRockSlabGeometry::profile(piece->difficulty);
        const double center = piece->challenge.obstacleDistanceMeters;
        QVERIFY(center + profile.startMeters
                >= piece->startDistanceMeters - 1e-9);
        QVERIFY(center + profile.endMeters
                <= piece->startDistanceMeters + piece->lengthMeters + 1e-9);
        QCOMPARE(piece->challenge.decisionDistanceMeters,
                 center + profile.startMeters);
        QVERIFY(piece->challenge.prepareDistanceMeters
                < piece->challenge.decisionDistanceMeters);
        QVERIFY(piece->challenge.decisionDistanceMeters
                - piece->challenge.prepareDistanceMeters <= 6.0 + 1e-9);
        QCOMPARE(piece->challenge.bypassStartDistanceMeters,
                 piece->challenge.bypassEndDistanceMeters);
        QCOMPARE(piece->challenge.bypassLateralMeters, 0.0);

        for (double local = profile.startMeters;
             local <= profile.endMeters; local += 0.05) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(road, center + local);
            QVERIFY(sample.ready);
            QVERIFY(std::abs(sample.surfaceOffsetMeters
                        - profile.surfaceOffsetMeters(local, 0.0)) < 1e-9);
            QVERIFY(std::abs(sample.center.halfWidthMeters
                        - profile.halfWidthMeters(local)) < 1e-9);
        }
    }

    void rockGardenProfileHasSocketedBuriedMainAndSafeLines()
    {
        const WorkoutGameRockGardenGeometryProfile profile =
                WorkoutGameRockGardenGeometry::profile(0.65);
        const WorkoutGameRockGardenGeometryProfile repeated =
                WorkoutGameRockGardenGeometry::profile(0.65);
        const WorkoutGameRockGardenGeometryProfile easiest =
                WorkoutGameRockGardenGeometry::profile(0.0);
        const WorkoutGameRockGardenGeometryProfile hardest =
                WorkoutGameRockGardenGeometry::profile(1.0);
        QVERIFY(profile.ready);
        QCOMPARE(profile.startMeters, -7.0);
        QCOMPARE(profile.activeStartMeters, -3.25);
        QCOMPARE(profile.activeEndMeters, 3.25);
        QCOMPARE(profile.endMeters, 7.0);
        QCOMPARE(profile.socketHalfWidthMeters, 0.68);
        QCOMPARE(profile.activeHalfWidthMeters, 1.35);
        QCOMPARE(profile.safeLineLateralMeters, 0.88);
        QVERIFY(profile.burialRatio >= 0.15);
        QVERIFY(profile.burialRatio <= 0.25);
        QCOMPARE(repeated.stones.size(), profile.stones.size());
        for (std::size_t index = 0; index < profile.stones.size(); ++index) {
            const WorkoutGameRockGardenStone &stone = profile.stones[index];
            const WorkoutGameRockGardenStone &sameStone =
                    repeated.stones[index];
            QCOMPARE(sameStone.forwardMeters, stone.forwardMeters);
            QCOMPARE(sameStone.lateralMeters, stone.lateralMeters);
            QCOMPARE(sameStone.forwardRadiusMeters,
                     stone.forwardRadiusMeters);
            QCOMPARE(sameStone.lateralRadiusMeters,
                     stone.lateralRadiusMeters);
            QCOMPARE(sameStone.heightMeters, stone.heightMeters);
            QCOMPARE(sameStone.yawRadians, stone.yawRadians);
            QCOMPARE(easiest.stones[index].forwardMeters,
                     hardest.stones[index].forwardMeters);
            QCOMPARE(easiest.stones[index].lateralMeters,
                     hardest.stones[index].lateralMeters);
            QVERIFY(easiest.stones[index].heightMeters
                    < hardest.stones[index].heightMeters);
        }
        QCOMPARE(profile.safeLineOffsetMeters(-6.0), 0.0);
        QCOMPARE(profile.safeLineOffsetMeters(0.0),
                 profile.safeLineLateralMeters);
        QCOMPARE(profile.safeLineOffsetMeters(6.0), 0.0);
        QCOMPARE(profile.halfWidthMeters(profile.startMeters),
                 profile.socketHalfWidthMeters);
        QCOMPARE(profile.halfWidthMeters(0.0),
                 profile.activeHalfWidthMeters);

        double maximumMainHeight = 0.0;
        double maximumSafeHeight = 0.0;
        int mainLineStones = 0;
        for (const WorkoutGameRockGardenStone &stone : profile.stones) {
            QVERIFY(stone.forwardMeters >= profile.activeStartMeters);
            QVERIFY(stone.forwardMeters <= profile.activeEndMeters);
            QVERIFY(stone.forwardRadiusMeters > 0.0);
            QVERIFY(stone.lateralRadiusMeters > 0.0);
            QVERIFY(stone.heightMeters > 0.0);
            if (std::abs(stone.lateralMeters)
                    < stone.lateralRadiusMeters) {
                ++mainLineStones;
            }
        }
        for (double local = profile.activeStartMeters;
             local <= profile.activeEndMeters; local += 0.01) {
            maximumMainHeight = std::max(
                    maximumMainHeight,
                    profile.surfaceOffsetMeters(local, 0.0));
            maximumSafeHeight = std::max(
                    maximumSafeHeight,
                    profile.surfaceOffsetMeters(
                        local, profile.safeLineLateralMeters));
        }
        QVERIFY(mainLineStones >= 6);
        QVERIFY(maximumMainHeight >= 0.12);
        QVERIFY(maximumMainHeight <= 0.24);
        QVERIFY(maximumSafeHeight <= 0.035);
        QVERIFY(maximumSafeHeight <= maximumMainHeight * 0.25);
    }

    void rockGardenRoadUsesTheCanonicalTyreContactProfile()
    {
        WorkoutGameCourse source;
        source.status = WorkoutGameCourseStatus::Ready;
        source.seed = 977u;
        source.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::Trail;
        section.terrain = WorkoutGameTerrainKind::RockGarden;
        section.durationMs = source.durationMs;
        section.lengthMeters = 76.0;
        section.targetWatts = 185.0;
        section.difficulty = 0.65;
        section.challengeCount = 1;
        source.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(source, 200.0);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameRockGardenGeometryProfile profile =
                WorkoutGameRockGardenGeometry::profile(piece->difficulty);
        const double center = piece->challenge.obstacleDistanceMeters;
        QCOMPARE(piece->challenge.decisionDistanceMeters,
                 center + profile.startMeters);
        QVERIFY(piece->challenge.decisionDistanceMeters
                - piece->challenge.prepareDistanceMeters > 0.0);
        QVERIFY(piece->challenge.decisionDistanceMeters
                - piece->challenge.prepareDistanceMeters <= 6.0 + 1e-9);
        QCOMPARE(piece->challenge.bypassStartDistanceMeters,
                 piece->challenge.bypassEndDistanceMeters);
        QCOMPARE(piece->challenge.bypassLateralMeters, 0.0);
        const WorkoutGameRoadSample middle =
                WorkoutGameRoadCourseBuilder::sample(road, center);
        QVERIFY(middle.ready);
        QCOMPARE(middle.center.halfWidthMeters,
                 profile.activeHalfWidthMeters);

        double maximumHeight = 0.0;
        for (double local = profile.startMeters;
             local <= profile.endMeters; local += 0.02) {
            const WorkoutGameRoadSample roadSample =
                    WorkoutGameRoadCourseBuilder::sample(road, center + local);
            QVERIFY(roadSample.ready);
            const double expected =
                    profile.surfaceOffsetMeters(local, 0.0);
            QVERIFY(std::abs(roadSample.surfaceOffsetMeters - expected) < 1e-9);
            maximumHeight = std::max(maximumHeight, expected);
        }
        QVERIFY(maximumHeight >= 0.12);
    }

    void rootProfileHasLevelSocketsAndAConnectedBuriedNetwork()
    {
        const WorkoutGameRootGeometryProfile profile =
                WorkoutGameRootGeometry::profile(0.65);
        QVERIFY(profile.ready);
        QVERIFY(profile.activeStartMeters > profile.startMeters);
        QVERIFY(profile.activeEndMeters < profile.endMeters);
        QCOMPARE(profile.surfaceOffsetMeters(profile.startMeters, 0.0), 0.0);
        QCOMPARE(profile.surfaceOffsetMeters(profile.activeStartMeters, 0.0), 0.0);
        QCOMPARE(profile.surfaceOffsetMeters(profile.activeEndMeters, 0.0), 0.0);
        QCOMPARE(profile.surfaceOffsetMeters(profile.endMeters, 0.0), 0.0);
        QCOMPARE(profile.halfWidthMeters(profile.startMeters),
                 profile.socketHalfWidthMeters);
        QCOMPARE(profile.halfWidthMeters(0.0),
                 profile.activeHalfWidthMeters);
        QCOMPARE(profile.safeLineOffsetMeters(-5.0), 0.0);
        QCOMPARE(profile.safeLineOffsetMeters(0.0),
                 profile.safeLineLateralMeters);
        QCOMPARE(profile.safeLineOffsetMeters(5.0), 0.0);
        QVERIFY(profile.segments.size() >= 7u);

        double maximumCenterHeight = 0.0;
        double maximumSafeLineHeight = 0.0;
        int centerCrossings = 0;
        for (const WorkoutGameRootSegment &root : profile.segments) {
            QVERIFY(root.startForwardMeters >= profile.activeStartMeters);
            QVERIFY(root.endForwardMeters <= profile.activeEndMeters);
            QVERIFY(root.startRadiusMeters > root.endRadiusMeters);
            QVERIFY(root.endRadiusMeters > 0.0);
            if (root.startLateralMeters * root.endLateralMeters <= 0.0) {
                ++centerCrossings;
            }
        }
        for (double local = profile.activeStartMeters;
             local <= profile.activeEndMeters; local += 0.01) {
            maximumCenterHeight = std::max(
                    maximumCenterHeight,
                    profile.surfaceOffsetMeters(local, 0.0));
            maximumSafeLineHeight = std::max(
                    maximumSafeLineHeight,
                    profile.surfaceOffsetMeters(
                        local, profile.safeLineLateralMeters));
        }
        QVERIFY(centerCrossings >= 5);
        QVERIFY(maximumCenterHeight >= 0.07);
        QVERIFY(maximumCenterHeight <= 0.16);
        QVERIFY(maximumSafeLineHeight <= 0.015);
        QVERIFY(maximumSafeLineHeight
                <= maximumCenterHeight * 0.25);
    }

    void rootRoadUsesTheCanonicalTyreContactProfile()
    {
        WorkoutGameCourse source;
        source.status = WorkoutGameCourseStatus::Ready;
        source.seed = 713u;
        source.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::Trail;
        section.terrain = WorkoutGameTerrainKind::Roots;
        section.durationMs = source.durationMs;
        section.lengthMeters = 70.0;
        section.targetWatts = 180.0;
        section.difficulty = 0.65;
        section.challengeCount = 1;
        source.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(source, 200.0);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameRootGeometryProfile profile =
                WorkoutGameRootGeometry::profile(piece->difficulty);
        const double center = piece->challenge.obstacleDistanceMeters;
        QCOMPARE(piece->challenge.decisionDistanceMeters,
                 center + profile.startMeters);
        QVERIFY(piece->challenge.decisionDistanceMeters
                - piece->challenge.prepareDistanceMeters > 0.0);
        QVERIFY(piece->challenge.decisionDistanceMeters
                - piece->challenge.prepareDistanceMeters <= 6.0 + 1e-9);
        QCOMPARE(piece->challenge.bypassStartDistanceMeters,
                 piece->challenge.bypassEndDistanceMeters);
        QCOMPARE(piece->challenge.bypassLateralMeters, 0.0);
        const WorkoutGameRoadSample middle =
                WorkoutGameRoadCourseBuilder::sample(road, center);
        QVERIFY(middle.ready);
        QCOMPARE(middle.center.halfWidthMeters,
                 profile.activeHalfWidthMeters);

        double maximumDifference = 0.0;
        for (double local = profile.startMeters;
             local <= profile.endMeters; local += 0.02) {
            const WorkoutGameRoadSample roadSample =
                    WorkoutGameRoadCourseBuilder::sample(road, center + local);
            QVERIFY(roadSample.ready);
            const double expected = profile.surfaceOffsetMeters(local, 0.0);
            QVERIFY(std::abs(roadSample.surfaceOffsetMeters - expected) < 1e-9);
            maximumDifference = std::max(maximumDifference, expected);
        }
        QVERIFY(maximumDifference >= 0.07);
    }

    void bermProfileHasLevelSocketsAndAConsistentBankDirection()
    {
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(0.65);
        QVERIFY(profile.ready);
        QCOMPARE(profile.startMeters, -3.87);
        QCOMPARE(profile.curveStartMeters, -2.62);
        QCOMPARE(profile.curveEndMeters, 2.62);
        QCOMPARE(profile.endMeters, 3.87);
        QCOMPARE(profile.socketHalfWidthMeters, 0.68 * 1.17);
        QCOMPARE(profile.activeHalfWidthMeters, 0.95);
        QCOMPARE(profile.headingProgress(profile.startMeters), 0.0);
        QCOMPARE(profile.headingProgress(profile.curveStartMeters), 0.0);
        QCOMPARE(profile.headingProgress(0.0), 0.5);
        QCOMPARE(profile.headingProgress(profile.curveEndMeters), 1.0);
        QCOMPARE(profile.headingProgress(profile.endMeters), 1.0);

        constexpr double RightTurnRadians = 1.0;
        constexpr double HalfWidthMeters = 0.95;
        const double rightBank = profile.bankRadians(
                0.0, RightTurnRadians);
        const double leftBank = profile.bankRadians(
                0.0, -RightTurnRadians);
        QVERIFY(rightBank < 0.0);
        QVERIFY(leftBank > 0.0);
        QCOMPARE(rightBank, -leftBank);
        QCOMPARE(profile.bankRadians(profile.startMeters,
                                     RightTurnRadians), 0.0);
        QCOMPARE(profile.bankRadians(profile.endMeters,
                                     RightTurnRadians), 0.0);
        const double rightTurnLeftEdge = profile.surfaceOffsetMeters(
                0.0, -HalfWidthMeters, HalfWidthMeters, RightTurnRadians);
        const double rightTurnCenter = profile.surfaceOffsetMeters(
                0.0, 0.0, HalfWidthMeters, RightTurnRadians);
        const double rightTurnRightEdge = profile.surfaceOffsetMeters(
                0.0, HalfWidthMeters, HalfWidthMeters, RightTurnRadians);
        QVERIFY(rightTurnLeftEdge > rightTurnCenter + 0.30);
        QCOMPARE(rightTurnCenter, 0.0);
        QVERIFY(rightTurnRightEdge < rightTurnCenter - 0.30);
        QCOMPARE(profile.halfWidthMeters(profile.startMeters),
                 profile.socketHalfWidthMeters);
        QCOMPARE(profile.halfWidthMeters(0.0),
                 profile.activeHalfWidthMeters);
        QCOMPARE(profile.safeLineLateralMeters(
                     profile.startMeters, RightTurnRadians), 0.0);
        QVERIFY(profile.safeLineLateralMeters(
                    0.0, RightTurnRadians) > 0.44);
        QCOMPARE(profile.safeLineLateralMeters(
                     profile.endMeters, RightTurnRadians), 0.0);
        QVERIFY(profile.riderWorldRollRadians(
                    0.0, RightTurnRadians, 5.0, false) < -0.25);
        QVERIFY(std::abs(profile.riderWorldRollRadians(
                    0.0, RightTurnRadians, 5.0, true))
                < std::abs(profile.riderWorldRollRadians(
                    0.0, RightTurnRadians, 5.0, false)));
    }

    void bermRoadSampleUsesTheSameLocalCurveAndBankProfile()
    {
        WorkoutGameCourse source;
        source.status = WorkoutGameCourseStatus::Ready;
        source.seed = 407u;
        source.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::Trail;
        section.terrain = WorkoutGameTerrainKind::Berm;
        section.durationMs = source.durationMs;
        section.lengthMeters = 70.0;
        section.targetWatts = 180.0;
        section.difficulty = 0.65;
        section.challengeCount = 1;
        section.visualVariant = 0u;
        source.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(source, 200.0);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(piece->difficulty);
        const double center = piece->challenge.obstacleDistanceMeters;
        QCOMPARE(std::abs(piece->turnRadians),
                 profile.turnMagnitudeRadians);
        const WorkoutGameRoadSample entry =
                WorkoutGameRoadCourseBuilder::sample(
                    road, center + profile.curveStartMeters);
        const WorkoutGameRoadSample middle =
                WorkoutGameRoadCourseBuilder::sample(road, center);
        const WorkoutGameRoadSample exit =
                WorkoutGameRoadCourseBuilder::sample(
                    road, center + profile.curveEndMeters);
        QVERIFY(entry.ready && middle.ready && exit.ready);
        QVERIFY(std::abs(entry.center.headingRadians
                         - piece->entry.headingRadians) < 1e-5);
        QVERIFY(std::abs(middle.center.headingRadians
                         - (piece->entry.headingRadians
                            + piece->turnRadians * 0.5)) < 1e-5);
        QVERIFY(std::abs(exit.center.headingRadians
                         - (piece->entry.headingRadians
                            + piece->turnRadians)) < 1e-5);
        QCOMPARE(entry.bermBankRadians, 0.0);
        QVERIFY(std::abs(middle.bermBankRadians) > 0.30);
        QCOMPARE(exit.bermBankRadians, 0.0);
        QVERIFY(middle.bermBankRadians * piece->turnRadians < 0.0);
    }

    void bermCenterlineIsContinuousMonotonicAndMirrored()
    {
        const auto buildBerm = [](std::uint32_t variant) {
            WorkoutGameCourse source;
            source.status = WorkoutGameCourseStatus::Ready;
            source.seed = 407u;
            source.durationMs = 60000;
            WorkoutGameSection section;
            section.feature = WorkoutGameFeature::Trail;
            section.terrain = WorkoutGameTerrainKind::Berm;
            section.durationMs = source.durationMs;
            section.lengthMeters = 400.0;
            section.targetWatts = 180.0;
            section.difficulty = 0.65;
            section.challengeCount = 1;
            section.visualVariant = variant;
            source.sections = {section};
            return WorkoutGameRoadCourseBuilder::build(source, 200.0);
        };
        const WorkoutGameRoadCourse left = buildBerm(0u);
        const WorkoutGameRoadCourse right = buildBerm(1u);
        QVERIFY(left.ready && right.ready);
        QCOMPARE(left.pieces.size(), std::size_t(1));
        QCOMPARE(right.pieces.size(), std::size_t(1));
        const WorkoutGameRoadPiece &leftPiece = left.pieces.front();
        const WorkoutGameRoadPiece &rightPiece = right.pieces.front();
        QVERIFY(leftPiece.challenge.enabled);
        QVERIFY(rightPiece.challenge.enabled);
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(leftPiece.difficulty);
        QVERIFY(leftPiece.challenge.obstacleDistanceMeters
                    + profile.startMeters > 0.0);
        QVERIFY(leftPiece.challenge.obstacleDistanceMeters
                    + profile.endMeters < left.totalLengthMeters);

        const double start = leftPiece.challenge.obstacleDistanceMeters
                + profile.startMeters;
        const double end = leftPiece.challenge.obstacleDistanceMeters
                + profile.endMeters;
        WorkoutGameRoadSample previous =
                WorkoutGameRoadCourseBuilder::sample(left, start);
        constexpr double StepMeters = 0.02;
        for (double distance = start + StepMeters;
             distance <= end; distance += StepMeters) {
            const WorkoutGameRoadSample current =
                    WorkoutGameRoadCourseBuilder::sample(left, distance);
            QVERIFY(current.ready);
            const double displacement = std::hypot(
                    current.center.xMeters - previous.center.xMeters,
                    current.center.zMeters - previous.center.zMeters);
            QVERIFY(displacement > StepMeters * 0.995);
            QVERIFY(displacement < StepMeters * 1.005);
            QVERIFY(current.center.headingRadians
                    <= previous.center.headingRadians + 1e-9);
            previous = current;
        }

        const WorkoutGameRoadSample leftExit =
                WorkoutGameRoadCourseBuilder::sample(left, left.totalLengthMeters);
        const WorkoutGameRoadSample rightExit =
                WorkoutGameRoadCourseBuilder::sample(right, right.totalLengthMeters);
        QVERIFY(leftExit.ready && rightExit.ready);
        QVERIFY(std::abs(leftExit.center.xMeters
                         + rightExit.center.xMeters) < 1e-6);
        QVERIFY(std::abs(leftExit.center.zMeters
                         - rightExit.center.zMeters) < 1e-6);
        QVERIFY(std::abs(leftExit.center.headingRadians
                         + rightExit.center.headingRadians) < 1e-9);
    }

    void invalidInputsFailClosed()
    {
        QVERIFY(!WorkoutGameRoadCourseBuilder::build({}, 200.0).ready);
        QVERIFY(!WorkoutGameRoadCourseBuilder::build(
                sampleCourse(), 0.0).ready);
        QVERIFY(!WorkoutGameRoadProjection::project({}, 0.0).ready);
    }

    void piecesJoinWithoutPositionHeadingOrWidthGaps()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        QVERIFY(road.ready);
        QVERIFY(road.pieces.size() >= 2u);
        for (std::size_t index = 1; index < road.pieces.size(); ++index) {
            const WorkoutGameRoadConnector &prior = road.pieces[index - 1].exit;
            const WorkoutGameRoadConnector &next = road.pieces[index].entry;
            QVERIFY(near(prior.xMeters, next.xMeters));
            QVERIFY(near(prior.zMeters, next.zMeters));
            QVERIFY(near(prior.elevationMeters, next.elevationMeters));
            QVERIFY(near(prior.headingRadians, next.headingRadians));
            QVERIFY(near(prior.halfWidthMeters, next.halfWidthMeters));
            QVERIFY(near(prior.gradePercent, next.gradePercent));
        }
    }

    void trailingCameraAnchorsTheTrackedRiderToTheChasePosition()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        WorkoutGameRoadProjectionConfig config;
        config.cameraTrailingDistanceMeters = 8.0;
        const double riderDistance = 30.0;
        const WorkoutGameRoadProjectionFrame projection =
                WorkoutGameRoadProjection::project(
                    road, riderDistance, config);
        QVERIFY(projection.ready);
        QVERIFY(projection.slices.back().worldDistanceMeters
                < riderDistance);
        const WorkoutGameRoadProjectedPoint rider =
                WorkoutGameRoadProjection::projectPoint(
                    projection, riderDistance, 0.0, 0.0);
        QVERIFY(rider.ready);
        QVERIFY(std::abs(rider.x - projection.riderScreenX) < 1e-6);
        QVERIFY(std::abs(rider.y - projection.riderScreenY) < 1e-6);
    }

    void logPieceOwnsTimedJumpGate()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        const WorkoutGameRoadPiece *jump = nullptr;
        for (const WorkoutGameRoadPiece &piece : road.pieces) {
            if (piece.challenge.enabled
                    && piece.terrain == WorkoutGameTerrainKind::LogOver) {
                jump = &piece;
                break;
            }
        }
        QVERIFY(jump != nullptr);
        QCOMPARE(jump->animation, WorkoutGameRoadAnimation::Jump);
        QCOMPARE(jump->challenge.profile.cue, WorkoutGameChallengeCue::Jump);
        QVERIFY(jump->challenge.prepareDistanceMeters
                < jump->challenge.decisionDistanceMeters);
        QVERIFY(jump->challenge.decisionDistanceMeters
                - jump->challenge.prepareDistanceMeters <= 6.0 + 1e-9);
        QVERIFY(jump->challenge.decisionDistanceMeters
                < jump->challenge.obstacleDistanceMeters);
        QCOMPARE(jump->challenge.decisionDistanceMeters,
                 jump->challenge.bypassStartDistanceMeters);
        QVERIFY(jump->challenge.bypassStartDistanceMeters
                < jump->challenge.obstacleDistanceMeters);
        QVERIFY(jump->challenge.bypassEndDistanceMeters
                > jump->challenge.obstacleDistanceMeters);
        QVERIFY(jump->challenge.bypassEndDistanceMeters
                - jump->challenge.bypassStartDistanceMeters >= 18.0 - 1e-9);
        const double obstacleBypassLateral = std::abs(
                WorkoutGameTrailBranch::lateralAt(
                    jump->challenge.obstacleDistanceMeters,
                    jump->challenge.bypassStartDistanceMeters,
                    jump->challenge.bypassEndDistanceMeters,
                    jump->challenge.bypassLateralMeters));
        QVERIFY2(obstacleBypassLateral >= 1.67,
                 "bypass does not clear the log and rider at the obstacle");
        const WorkoutGameFeatureGeometryProfile featureGeometry =
                WorkoutGameFeatureGeometry::profile(
                    jump->terrain, jump->difficulty);
        QVERIFY(featureGeometry.ready);
        QVERIFY(jump->challenge.bypassEndDistanceMeters
                >= jump->challenge.obstacleDistanceMeters
                    + featureGeometry.endMeters + 10.0 - 1e-9);
        QVERIFY(jump->challenge.obstacleDistanceMeters
                >= jump->startDistanceMeters);
        QVERIFY(jump->challenge.obstacleDistanceMeters
                <= jump->startDistanceMeters + jump->lengthMeters);
        const WorkoutGameRoadSample obstacle =
                WorkoutGameRoadCourseBuilder::sample(
                    road, jump->challenge.obstacleDistanceMeters);
        const WorkoutGameRoadSample approach =
                WorkoutGameRoadCourseBuilder::sample(
                    road, jump->challenge.obstacleDistanceMeters - 1.1);
        QVERIFY(obstacle.ready);
        QVERIFY(approach.ready);
        QVERIFY(obstacle.center.elevationMeters
                > approach.center.elevationMeters + 0.15);
        QVERIFY(std::isfinite(obstacle.center.gradePercent));
    }

    void bunnyHopUsesACompactPreloadWindowOnOrdinaryGround()
    {
        WorkoutGameCourse course = sampleCourse();
        course.sections.back().terrain = WorkoutGameTerrainKind::BunnyHop;
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        const auto bunny = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.challenge.enabled
                            && piece.terrain
                                == WorkoutGameTerrainKind::BunnyHop;
                });
        QVERIFY(bunny != road.pieces.end());
        const double preloadMeters =
                bunny->challenge.decisionDistanceMeters
                - bunny->challenge.prepareDistanceMeters;
        QVERIFY(preloadMeters > 0.0);
        QVERIFY(preloadMeters <= 3.0 + 1e-9);
        QVERIFY(bunny->challenge.obstacleDistanceMeters
                - bunny->challenge.decisionDistanceMeters <= 4.0 + 1e-9);
        const WorkoutGameRoadSample obstacle =
                WorkoutGameRoadCourseBuilder::sample(
                    road, bunny->challenge.obstacleDistanceMeters);
        QVERIFY(obstacle.ready);
        QCOMPARE(obstacle.surfaceOffsetMeters, 0.0);
    }

    void dropRoadDefinesAnActualGapAndLowerLanding()
    {
        WorkoutGameCourse course = sampleCourse();
        course.sections.back().terrain = WorkoutGameTerrainKind::Drop;
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        const auto drop = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.challenge.enabled
                            && piece.terrain == WorkoutGameTerrainKind::Drop;
                });
        QVERIFY(drop != road.pieces.end());
        const double lip = drop->challenge.obstacleDistanceMeters;
        const WorkoutGameRoadSample approach =
                WorkoutGameRoadCourseBuilder::sample(road, lip - 0.1);
        const WorkoutGameRoadSample gap =
                WorkoutGameRoadCourseBuilder::sample(road, lip + 0.5);
        const WorkoutGameRoadSample landing =
                WorkoutGameRoadCourseBuilder::sample(road, lip + 1.25);
        const WorkoutGameRoadSample runout =
                WorkoutGameRoadCourseBuilder::sample(road, lip + 4.0);
        QVERIFY(approach.ready && gap.ready && landing.ready && runout.ready);
        QVERIFY(approach.rideableSurface);
        QVERIFY(!gap.rideableSurface);
        QVERIFY(landing.rideableSurface);
        QVERIFY(runout.rideableSurface);
        QCOMPARE(approach.surfaceOffsetMeters, 0.0);
        QVERIFY(landing.surfaceOffsetMeters < -0.35);
        QCOMPARE(runout.surfaceOffsetMeters, landing.surfaceOffsetMeters);
    }

    void rollersUseThreeRoundedCanonicalCrestsAndTroughs()
    {
        WorkoutGameCourse course = sampleCourse();
        course.sections.back().terrain = WorkoutGameTerrainKind::Rollers;
        course.sections.back().difficulty = 0.5;
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        const auto rollers = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.challenge.enabled
                            && piece.terrain
                                == WorkoutGameTerrainKind::Rollers;
                });
        QVERIFY(rollers != road.pieces.end());
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    rollers->terrain, rollers->difficulty);
        QVERIFY(profile.ready);
        QCOMPARE(profile.startMeters, -5.25);
        QCOMPARE(profile.plateauStartMeters, -4.5);
        QCOMPARE(profile.plateauEndMeters, 4.5);
        QCOMPARE(profile.endMeters, 5.25);
        QCOMPARE(profile.heightMeters, 0.24);
        QCOMPARE(rollers->challenge.bypassStartDistanceMeters,
                 rollers->challenge.bypassEndDistanceMeters);
        QCOMPARE(rollers->challenge.bypassLateralMeters, 0.0);

        const double obstacle = rollers->challenge.obstacleDistanceMeters;
        for (double local : {-5.25, -4.5, -1.5, 1.5, 4.5, 5.25}) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, obstacle + local);
            QVERIFY(sample.ready);
            QVERIFY2(std::abs(sample.surfaceOffsetMeters) < 1e-9,
                     qPrintable(QStringLiteral(
                         "roller trough at %1 m was %2 m")
                         .arg(local).arg(sample.surfaceOffsetMeters)));
        }
        for (double local : {-3.0, 0.0, 3.0}) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, obstacle + local);
            QVERIFY(sample.ready);
            QVERIFY2(std::abs(sample.surfaceOffsetMeters
                              - profile.heightMeters) < 1e-9,
                     qPrintable(QStringLiteral(
                         "roller crest at %1 m was %2 m")
                         .arg(local).arg(sample.surfaceOffsetMeters)));
        }
    }

    void generationAndSamplingAreDeterministic()
    {
        const WorkoutGameRoadCourse first =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        const WorkoutGameRoadCourse second =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        QCOMPARE(first.pieces.size(), second.pieces.size());
        QCOMPARE(first.totalLengthMeters, second.totalLengthMeters);
        for (double distance = 0.0;
             distance <= first.totalLengthMeters; distance += 3.25) {
            const WorkoutGameRoadSample left =
                    WorkoutGameRoadCourseBuilder::sample(first, distance);
            const WorkoutGameRoadSample right =
                    WorkoutGameRoadCourseBuilder::sample(second, distance);
            QCOMPARE(left.center.xMeters, right.center.xMeters);
            QCOMPARE(left.center.zMeters, right.center.zMeters);
            QCOMPARE(left.center.elevationMeters,
                     right.center.elevationMeters);
        }
    }

    void workoutTimelineIsContinuousAndMonotonic()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        QCOMPARE(road.timeline.size(), std::size_t(2));
        QVERIFY(near(road.timeline[0].endDistanceMeters,
                     road.timeline[1].startDistanceMeters));

        double previousDistance = -1.0;
        for (std::int64_t timeMs = -1000; timeMs <= 31000; timeMs += 17) {
            const WorkoutGameRoadTimelineSample sample =
                    WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(
                            road, timeMs);
            QVERIFY(sample.ready);
            QVERIFY(sample.distanceMeters + 1e-9 >= previousDistance);
            previousDistance = sample.distanceMeters;
        }
        const WorkoutGameRoadTimelineSample boundary =
                WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(road, 15000);
        QCOMPARE(boundary.sourceSectionIndex, std::size_t(1));
        QCOMPARE(boundary.sectionProgress, 0.0);
        QVERIFY(near(boundary.distanceMeters,
                     road.timeline[1].startDistanceMeters));
        const WorkoutGameRoadTimelineSample end =
                WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(road, 40000);
        QCOMPARE(end.distanceMeters, road.totalLengthMeters);
        QCOMPARE(end.sectionProgress, 1.0);
    }

    void savedDistanceCourseLengthsAreNotReestimated()
    {
        WorkoutGameCourse course = sampleCourse();
        course.sections[0].lengthMeters = 114.86038315951464;
        course.sections[1].lengthMeters = 154.99442656317188;
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 250.0);

        QVERIFY(road.ready);
        QCOMPARE(road.timeline[0].startDistanceMeters, 0.0);
        QCOMPARE(road.timeline[0].endDistanceMeters,
                 course.sections[0].lengthMeters);
        QCOMPARE(road.timeline[1].startDistanceMeters,
                 course.sections[0].lengthMeters);
        QCOMPARE(road.totalLengthMeters,
                 course.sections[0].lengthMeters
                    + course.sections[1].lengthMeters);
    }

    void climbAndTurnAreVisibleInWorldGeometry()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        const WorkoutGameRoadPiece &first = road.pieces.front();
        QVERIFY(first.exit.elevationMeters > first.entry.elevationMeters);
        QVERIFY(std::abs(first.exit.headingRadians
                - first.entry.headingRadians) > 0.01);
        QVERIFY(std::abs(first.exit.xMeters - first.entry.xMeters) > 0.01);
    }

    void trailUsesSingletrackWidthAndContinuousRelief()
    {
        WorkoutGameCourse course = sampleCourse();
        for (WorkoutGameSection &section : course.sections) {
            section.terrain = WorkoutGameTerrainKind::SmoothTrail;
            section.gradePercent = 0.0;
            section.challengeCount = 0;
        }
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        QVERIFY(road.ready);

        double minimumElevation = 1e9;
        double maximumElevation = -1e9;
        double maximumHalfWidth = 0.0;
        for (double distance = 0.0;
             distance <= road.totalLengthMeters; distance += 0.5) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(road, distance);
            QVERIFY(sample.ready);
            minimumElevation = std::min(
                    minimumElevation, sample.center.elevationMeters);
            maximumElevation = std::max(
                    maximumElevation, sample.center.elevationMeters);
            maximumHalfWidth = std::max(
                    maximumHalfWidth, sample.center.halfWidthMeters);
        }
        QVERIFY2(maximumHalfWidth <= 0.75,
                 "the generated trail is wider than a singletrack");
        QVERIFY2(maximumElevation - minimumElevation >= 0.20,
                 "smooth trail relief is too flat to read while riding");
        QVERIFY(std::abs(road.pieces.front().turnRadians) > 0.02);
    }

    void jumpEffortWindowIsCloseToObstacle()
    {
        for (std::int64_t durationMs : {15000, 120000}) {
            WorkoutGameCourse course;
            course.status = WorkoutGameCourseStatus::Ready;
            course.seed = 123u;
            course.durationMs = durationMs;
            WorkoutGameSection section;
            section.feature = WorkoutGameFeature::SprintJump;
            section.terrain = WorkoutGameTerrainKind::LogOver;
            section.durationMs = durationMs;
            section.targetWatts = 260.0;
            section.difficulty = 0.5;
            section.challengeCount = 1;
            course.sections.push_back(section);
            const WorkoutGameRoadCourse road =
                    WorkoutGameRoadCourseBuilder::build(course, 200.0);
            const auto jump = std::find_if(
                    road.pieces.begin(), road.pieces.end(),
                    [](const WorkoutGameRoadPiece &piece) {
                        return piece.challenge.enabled;
                    });
            QVERIFY(jump != road.pieces.end());
            QVERIFY(jump->challenge.obstacleDistanceMeters
                    - jump->challenge.prepareDistanceMeters <= 10.0);
            QVERIFY(jump->challenge.obstacleDistanceMeters
                    - jump->challenge.decisionDistanceMeters <= 4.0 + 1e-9);
        }
    }

    void projectionUsesDepthInsteadOfSidewaysTravel()
    {
        WorkoutGameCourse straightCourse = sampleCourse();
        for (WorkoutGameSection &section : straightCourse.sections) {
            section.terrain = WorkoutGameTerrainKind::SmoothTrail;
            section.visualVariant = 0u;
            section.gradePercent = 0.0;
            section.challengeCount = 0;
        }
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(straightCourse, 200.0);
        const WorkoutGameRoadProjectionFrame frame =
                WorkoutGameRoadProjection::project(road, 0.0);
        QVERIFY(frame.ready);
        QVERIFY(frame.slices.size() >= 90u);
        const double center = 1280.0 * 0.5;
        for (const WorkoutGameRoadProjectedSlice &slice : frame.slices) {
            QVERIFY(std::abs(slice.centerX - center) < 1.0);
        }
        QVERIFY(frame.slices.front().centerY
                < frame.slices.back().centerY);
        QVERIFY(frame.slices.front().halfWidthPixels
                < frame.slices.back().halfWidthPixels);
        QCOMPARE(frame.riderScreenX, center);
    }

    void projectionReportsAndDrawsVisibleClimbRelief()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 19u;
        course.durationMs = 180000;
        WorkoutGameSection climb;
        climb.feature = WorkoutGameFeature::Climb;
        climb.terrain = WorkoutGameTerrainKind::Climb;
        climb.durationMs = course.durationMs;
        climb.targetWatts = 230.0;
        climb.gradePercent = 7.0;
        climb.difficulty = 0.7;
        course.sections = {climb};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 190.0);
        const WorkoutGameRoadProjectionFrame frame =
                WorkoutGameRoadProjection::project(road, 20.0);

        QVERIFY(frame.ready);
        QVERIFY(frame.renderedGradePercent > 5.0);
        QVERIFY(frame.visibleElevationChangeMeters > 4.0);
        QVERIFY(frame.slices.front().centerY
                < frame.slices.back().centerY - 20.0);
    }

    void projectionUsesCanonicalTechnicalSurfaceHeight()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 29u;
        course.durationMs = 60000;
        WorkoutGameSection roots;
        roots.feature = WorkoutGameFeature::Trail;
        roots.terrain = WorkoutGameTerrainKind::Roots;
        roots.durationMs = course.durationMs;
        roots.targetWatts = 170.0;
        roots.difficulty = 1.0;
        course.sections = {roots};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 190.0);
        const WorkoutGameRoadProjectionFrame frame =
                WorkoutGameRoadProjection::project(road, 0.0);
        QVERIFY(frame.ready);

        bool sawTechnicalRelief = false;
        for (const WorkoutGameRoadProjectedSlice &slice : frame.slices) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, slice.worldDistanceMeters);
            QCOMPARE(slice.surfaceElevationMeters,
                     sample.center.elevationMeters);
            sawTechnicalRelief = sawTechnicalRelief
                    || sample.surfaceOffsetMeters > 0.005;
        }
        QVERIFY(sawTechnicalRelief);
    }

    void forestTrailHasVisibleRollingReliefBetweenConnectors()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 912u;
        course.durationMs = 90000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::Trail;
        section.terrain = WorkoutGameTerrainKind::SmoothTrail;
        section.durationMs = course.durationMs;
        section.targetWatts = 180.0;
        section.difficulty = 0.8;
        course.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 190.0);
        QVERIFY(road.ready);
        double maximumRelief = 0.0;
        for (const WorkoutGameRoadPiece &piece : road.pieces) {
            const double start = piece.startDistanceMeters;
            const double end = start + piece.lengthMeters;
            const double startElevation =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, start).surfaceElevationMeters();
            const double endElevation =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, end).surfaceElevationMeters();
            for (int sampleIndex = 1; sampleIndex < 8; ++sampleIndex) {
                const double progress = double(sampleIndex) / 8.0;
                const double elevation =
                        WorkoutGameRoadCourseBuilder::sample(
                            road, start + progress * piece.lengthMeters)
                            .surfaceElevationMeters();
                const double baseline = startElevation
                        + (endElevation - startElevation) * progress;
                maximumRelief = std::max(
                        maximumRelief, std::abs(elevation - baseline));
            }
        }
        QVERIFY2(maximumRelief > 0.8,
                 "terrain relief is still visually too shallow");
    }

    void jumpSurfaceOffsetIsAppliedExactlyOnce()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 730u;
        course.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::Tabletop;
        section.durationMs = course.durationMs;
        section.targetWatts = 260.0;
        section.difficulty = 1.0;
        section.challengeCount = 1;
        course.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const double obstacle = piece->challenge.obstacleDistanceMeters;
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece->terrain, piece->difficulty);
        QVERIFY(profile.ready);
        const WorkoutGameRoadProjectionFrame frame =
                WorkoutGameRoadProjection::project(road, obstacle - 8.0);
        bool sawFeatureSurface = false;
        for (const WorkoutGameRoadProjectedSlice &slice : frame.slices) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, slice.worldDistanceMeters);
            QCOMPARE(slice.surfaceElevationMeters,
                     sample.surfaceElevationMeters());
            sawFeatureSurface = sawFeatureSurface
                    || sample.surfaceOffsetMeters
                        >= profile.heightMeters * 0.95;
        }
        QVERIFY(sawFeatureSurface);
        QVERIFY(frame.slices.size() >= 16u);
    }

    void horizonIsShapedDeterministicAndScrollsSmoothly()
    {
        const WorkoutGameHorizonSnapshot first =
                WorkoutGameHorizon::build(123u, 100.0, 32);
        const WorkoutGameHorizonSnapshot same =
                WorkoutGameHorizon::build(123u, 100.0, 32);
        const WorkoutGameHorizonSnapshot moved =
                WorkoutGameHorizon::build(123u, 100.1, 32);
        QVERIFY(first.ready);
        QCOMPARE(first.farRidgeY, same.farRidgeY);
        QCOMPARE(first.nearRidgeY, same.nearRidgeY);
        double minimum = 1.0;
        double maximum = 0.0;
        for (std::size_t index = 0; index < first.nearRidgeY.size(); ++index) {
            minimum = std::min(minimum, first.nearRidgeY[index]);
            maximum = std::max(maximum, first.nearRidgeY[index]);
            QVERIFY(std::abs(first.nearRidgeY[index]
                    - moved.nearRidgeY[index]) < 0.002);
        }
        QVERIFY(maximum - minimum > 0.06);
    }

    void tinyDistanceChangesProduceContinuousProjection()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        const WorkoutGameRoadProjectionFrame first =
                WorkoutGameRoadProjection::project(road, 10.0);
        const WorkoutGameRoadProjectionFrame next =
                WorkoutGameRoadProjection::project(road, 10.01);
        QVERIFY(first.ready);
        QVERIFY(next.ready);
        QCOMPARE(first.slices.size(), next.slices.size());
        for (std::size_t index = 0; index < first.slices.size(); ++index) {
            QVERIFY(std::abs(first.slices[index].centerX
                    - next.slices[index].centerX) < 2.0);
            QVERIFY(std::abs(first.slices[index].centerY
                    - next.slices[index].centerY) < 2.0);
        }
    }

    void crestOcclusionMarksRoadHiddenByNearerTerrain()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(crestCourse(), 200.0);
        WorkoutGameRoadProjectionConfig config;
        config.visibleDistanceMeters = 120.0;
        const WorkoutGameRoadProjectionFrame frame =
                WorkoutGameRoadProjection::project(road, 0.0, config);
        QVERIFY(frame.ready);

        bool hiddenSlice = false;
        double priorOcclusion = config.viewportHeight;
        for (auto slice = frame.slices.rbegin();
             slice != frame.slices.rend(); ++slice) {
            QVERIFY(slice->occlusionY <= priorOcclusion + 1e-9);
            priorOcclusion = slice->occlusionY;
            hiddenSlice = hiddenSlice
                    || slice->centerY > slice->occlusionY + 1e-6;
        }
        QVERIFY2(hiddenSlice, "the valley behind the crest was not occluded");
    }

    void explicitCameraElevationMovesProjectionSmoothly()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        WorkoutGameRoadProjectionConfig lowConfig;
        lowConfig.cameraElevationMeters = 2.0;
        WorkoutGameRoadProjectionConfig highConfig = lowConfig;
        highConfig.cameraElevationMeters = 2.1;
        const WorkoutGameRoadProjectionFrame low =
                WorkoutGameRoadProjection::project(road, 10.0, lowConfig);
        const WorkoutGameRoadProjectionFrame high =
                WorkoutGameRoadProjection::project(road, 10.0, highConfig);
        QVERIFY(low.ready);
        QVERIFY(high.ready);
        QCOMPARE(low.slices.size(), high.slices.size());
        for (std::size_t index = 0; index < low.slices.size(); ++index) {
            QVERIFY(high.slices[index].centerY > low.slices[index].centerY);
        }
    }

    void projectionPreservesNarrowFeatureBreakpoints()
    {
        WorkoutGameCourse source;
        source.status = WorkoutGameCourseStatus::Ready;
        source.seed = 551u;
        source.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::LogOver;
        section.durationMs = source.durationMs;
        section.targetWatts = 260.0;
        section.difficulty = 0.5;
        section.challengeCount = 1;
        source.sections.push_back(section);
        const WorkoutGameRoadCourse course =
                WorkoutGameRoadCourseBuilder::build(source, 200.0);
        QVERIFY(course.ready);
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());
        const double obstacle = piece->challenge.obstacleDistanceMeters;
        const WorkoutGameRoadProjectionFrame projection =
                WorkoutGameRoadProjection::project(course, obstacle - 10.0);
        QVERIFY(projection.ready);
        const auto obstacleSlice = std::find_if(
                projection.slices.begin(), projection.slices.end(),
                [obstacle](const WorkoutGameRoadProjectedSlice &slice) {
                    return std::abs(slice.worldDistanceMeters - obstacle) < 1e-7;
                });
        QVERIFY(obstacleSlice != projection.slices.end());
        QVERIFY(obstacleSlice->surfaceOffsetMeters > 0.4);
    }

    void featureSurfaceContinuesAcrossPieceBoundary()
    {
        WorkoutGameCourse source;
        source.status = WorkoutGameCourseStatus::Ready;
        source.seed = 667u;
        source.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::Tabletop;
        section.durationMs = source.durationMs;
        section.targetWatts = 280.0;
        section.difficulty = 0.7;
        section.challengeCount = 1;
        source.sections.push_back(section);
        WorkoutGameRoadCourse selected =
                WorkoutGameRoadCourseBuilder::build(source, 200.0);
        QVERIFY(selected.pieces.size() >= 2u);
        const auto generatedChallenge = std::find_if(
                selected.pieces.begin(), selected.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.challenge.enabled;
                });
        QVERIFY(generatedChallenge != selected.pieces.end());
        WorkoutGameRoadChallengeGate challenge = generatedChallenge->challenge;
        for (WorkoutGameRoadPiece &piece : selected.pieces) {
            piece.challenge.enabled = false;
        }
        WorkoutGameRoadPiece &challengePiece = selected.pieces.front();
        challengePiece.challenge = challenge;
        challengePiece.challenge.enabled = true;
        const double boundary = challengePiece.startDistanceMeters
                + challengePiece.lengthMeters;
        challengePiece.challenge.obstacleDistanceMeters = boundary;
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    challengePiece.terrain, challengePiece.difficulty);
        QVERIFY(profile.ready);
        const WorkoutGameRoadSample before =
                WorkoutGameRoadCourseBuilder::sample(selected, boundary - 0.01);
        const WorkoutGameRoadSample after =
                WorkoutGameRoadCourseBuilder::sample(selected, boundary + 0.01);
        QVERIFY(before.surfaceOffsetMeters > 0.05);
        QVERIFY(after.surfaceOffsetMeters > 0.05);
        QVERIFY(std::abs(before.surfaceOffsetMeters
                - after.surfaceOffsetMeters) < 0.05);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameRoadCourse)
#include "testWorkoutGameRoadCourse.moc"
