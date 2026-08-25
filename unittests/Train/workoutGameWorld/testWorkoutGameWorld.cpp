/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameWorld.h"
#include "Train/WorkoutGameBermGeometry.h"
#include "Train/WorkoutGameRootGeometry.h"
#include "Train/WorkoutGameRockGardenGeometry.h"
#include "Train/WorkoutGameRoadCourse.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <limits>

class TestWorkoutGameWorld : public QObject
{
    Q_OBJECT

private slots:
    void authoritativeDistanceUsesTheRenderedCourseSurface()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 91u;
        course.durationMs = 12000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::LogOver;
        section.durationMs = course.durationMs;
        section.targetWatts = 240.0;
        section.gradePercent = 12.0;
        section.difficulty = 0.6;
        section.challengeCount = 1;
        course.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        QVERIFY(road.ready);

        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(road));
        WorkoutGamePhysicsInput input;
        input.terrain = section.terrain;
        input.desiredSpeedMetersPerSecond = 5.0;
        input.effortRatio = 1.0;
        double previousDistance = 0.0;
        std::uint64_t physicsGeneration = 0;
        for (int tick = 0; tick <= 100; ++tick) {
            input.workoutTimeMs = tick * 20;
            input.courseDistanceMeters = tick * 0.1;
            const WorkoutGameRoadSample surface =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, input.courseDistanceMeters);
            input.gradePercent = surface.center.gradePercent;
            const WorkoutGameWorldSnapshot frame = physics.update(input);
            QVERIFY(frame.ready);
            QCOMPARE(frame.rider.distanceMeters,
                     input.courseDistanceMeters);
            QVERIFY(frame.rider.distanceMeters >= previousDistance);
            QVERIFY(std::isfinite(frame.rider.elevationMeters));
            QVERIFY(std::isfinite(frame.rider.clearanceMeters));
            if (tick >= 50) {
                QVERIFY2(std::abs(frame.rider.elevationMeters
                            - surface.center.elevationMeters) < 0.6,
                         "vehicle elevation diverged from the canonical surface");
            }
            if (tick == 0) physicsGeneration = frame.generation;
            QCOMPARE(frame.generation, physicsGeneration);
            previousDistance = frame.rider.distanceMeters;
        }
    }

    void authoritativePhysicsUsesTheSameTechnicalSurfaceAsRendering()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 171u;
        course.durationMs = 60000;
        WorkoutGameSection roots;
        roots.feature = WorkoutGameFeature::Trail;
        roots.terrain = WorkoutGameTerrainKind::Roots;
        roots.durationMs = course.durationMs;
        roots.targetWatts = 180.0;
        roots.difficulty = 1.0;
        roots.challengeCount = 1;
        course.sections = {roots};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 190.0);
        QVERIFY(road.ready);

        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(road));
        WorkoutGamePhysicsInput input;
        input.terrain = roots.terrain;
        input.desiredSpeedMetersPerSecond = 4.0;
        input.effortRatio = 1.0;
        double largestSurfaceOffset = 0.0;
        double largestReportedDifference = 0.0;
        for (int tick = 0; tick <= 600; ++tick) {
            input.workoutTimeMs = tick * 20;
            input.courseDistanceMeters = tick * 0.05;
            const WorkoutGameRoadSample surface =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, input.courseDistanceMeters);
            const WorkoutGameWorldSnapshot frame = physics.update(input);
            QVERIFY(frame.ready);
            const double renderedSurface = surface.surfaceElevationMeters();
            largestSurfaceOffset = std::max(
                    largestSurfaceOffset, surface.surfaceOffsetMeters);
            largestReportedDifference = std::max(
                    largestReportedDifference,
                    std::abs(frame.surfaceElevationMeters - renderedSurface));
        }
        QVERIFY(largestSurfaceOffset > 0.005);
        QVERIFY(largestReportedDifference < 0.02);
    }

    void safeBypassDoesNotCollideWithTheVisibleJumpMesh()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 451u;
        course.durationMs = 20000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::Tabletop;
        section.durationMs = course.durationMs;
        section.targetWatts = 230.0;
        section.difficulty = 0.8;
        section.challengeCount = 1;
        course.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        QVERIFY(road.ready);
        const auto challengePiece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.challenge.enabled;
                });
        QVERIFY(challengePiece != road.pieces.end());
        const double obstacle =
                challengePiece->challenge.obstacleDistanceMeters;
        const WorkoutGameRoadSample obstacleSurface =
                WorkoutGameRoadCourseBuilder::sample(road, obstacle);
        QVERIFY(obstacleSurface.nonPhysicalFeatureOffsetMeters > 0.4);

        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(road));
        WorkoutGamePhysicsInput input;
        input.terrain = section.terrain;
        input.desiredSpeedMetersPerSecond = 5.0;
        input.effortRatio = 0.5;
        input.forceGroundFollowing = true;
        const double start = std::max(0.0, obstacle - 6.0);
        double maximumAirHeight = 0.0;
        for (int tick = 0; tick <= 150; ++tick) {
            input.workoutTimeMs = tick * 20;
            input.courseDistanceMeters = start + tick * 0.1;
            const WorkoutGameRoadSample surface =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, input.courseDistanceMeters);
            const WorkoutGameWorldSnapshot frame = physics.update(input);
            QVERIFY(frame.ready);
            const double physicalElevation = surface.surfaceElevationMeters()
                    - surface.nonPhysicalFeatureOffsetMeters;
            QVERIFY(std::abs(frame.surfaceElevationMeters
                             - physicalElevation) < 0.03);
            if (frame.rider.airborne) {
                maximumAirHeight = std::max(
                        maximumAirHeight, frame.rider.airHeightMeters());
            }
        }
        QVERIFY(maximumAirHeight < 0.08);
    }

    void ordinaryRollingReliefKeepsRiderGrounded()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 4015825171u;
        course.durationMs = 48000;
        WorkoutGameSection roots;
        roots.feature = WorkoutGameFeature::Trail;
        roots.terrain = WorkoutGameTerrainKind::Roots;
        roots.durationMs = course.durationMs;
        roots.targetWatts = 177.0;
        roots.gradePercent = 0.1175;
        roots.difficulty = 0.235;
        roots.challengeCount = 0;
        course.sections = {roots};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 190.0);
        QVERIFY(road.ready);

        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(road));
        WorkoutGamePhysicsInput input;
        input.terrain = roots.terrain;
        input.desiredSpeedMetersPerSecond = 20.0;
        input.effortRatio = 1.0;
        double maximumAirHeightMeters = 0.0;
        double maximumAirHeightDistanceMeters = 0.0;
        double maximumAirHeightGradePercent = 0.0;
        const int ticks = int((road.totalLengthMeters - 1.0) / 0.4);
        for (int tick = 0; tick <= ticks; ++tick) {
            input.workoutTimeMs = tick * 20;
            input.courseDistanceMeters = tick * 0.4;
            const WorkoutGameRoadSample surface =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, input.courseDistanceMeters);
            input.gradePercent = surface.center.gradePercent;
            const WorkoutGameWorldSnapshot frame = physics.update(input);
            QVERIFY(frame.ready);
            if (tick > 50) {
                const double airHeight = frame.rider.airborne
                        ? frame.rider.airHeightMeters() : 0.0;
                if (airHeight > maximumAirHeightMeters) {
                    maximumAirHeightMeters = airHeight;
                    maximumAirHeightDistanceMeters = input.courseDistanceMeters;
                    maximumAirHeightGradePercent = surface.center.gradePercent;
                }
            }
        }
        const QByteArray failure = QStringLiteral(
                "ordinary trail relief raised the rider %1 m at %2 m, "
                "grade %3%")
                .arg(maximumAirHeightMeters)
                .arg(maximumAirHeightDistanceMeters)
                .arg(maximumAirHeightGradePercent)
                .toUtf8();
        QVERIFY2(maximumAirHeightMeters < 0.08, failure.constData());
    }

    void terrainProfilesAreDeterministicAndPeriodic()
    {
        for (WorkoutGameTerrainKind terrain : {
                WorkoutGameTerrainKind::SmoothTrail,
                WorkoutGameTerrainKind::Roots,
                WorkoutGameTerrainKind::Rollers,
                WorkoutGameTerrainKind::Climb,
                WorkoutGameTerrainKind::RockGarden,
                WorkoutGameTerrainKind::BunnyHop,
                WorkoutGameTerrainKind::Drop}) {
            const double first = WorkoutGamePhysics::terrainHeight(
                    terrain, 17.25, 0.0, 0.7, 41u);
            const double repeated = WorkoutGamePhysics::terrainHeight(
                    terrain, 97.25, 0.0, 0.7, 41u);
            QVERIFY(std::isfinite(first));
            QCOMPARE(first, repeated);
        }

        const double root = WorkoutGamePhysics::terrainHeight(
                WorkoutGameTerrainKind::Roots, 17.25, 0.0, 0.7, 41u);
        const double roller = WorkoutGamePhysics::terrainHeight(
                WorkoutGameTerrainKind::Rollers, 17.25, 0.0, 0.7, 41u);
        QVERIFY(root != roller);
    }

    void fixedInputsProduceDeterministicVehiclePose()
    {
        WorkoutGamePhysics first;
        WorkoutGamePhysics second;
        QVERIFY(first.configure(77u));
        QVERIFY(second.configure(77u));

        WorkoutGamePhysicsInput input;
        input.terrain = WorkoutGameTerrainKind::Roots;
        input.desiredSpeedMetersPerSecond = 6.0;
        input.difficulty = 0.7;
        input.effortRatio = 1.0;
        for (int time = 0; time <= 5000; time += 50) {
            input.workoutTimeMs = time;
            const WorkoutGameWorldSnapshot a = first.update(input);
            const WorkoutGameWorldSnapshot b = second.update(input);
            QCOMPARE(a.rider.distanceMeters, b.rider.distanceMeters);
            QCOMPARE(a.rider.elevationMeters, b.rider.elevationMeters);
            QCOMPARE(a.rider.pitchDegrees, b.rider.pitchDegrees);
            QCOMPARE(a.rider.rearSuspension, b.rider.rearSuspension);
            QCOMPARE(a.rider.frontSuspension, b.rider.frontSuspension);
            QCOMPARE(a.rider.airborne, b.rider.airborne);
        }
    }

    void forwardRideNeverPublishesBackwardCourseProgress()
    {
        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(997u));
        WorkoutGamePhysicsInput input;
        input.desiredSpeedMetersPerSecond = 6.0;
        input.effortRatio = 1.0;

        double priorDistance = 0.0;
        const WorkoutGameTerrainKind terrains[] = {
            WorkoutGameTerrainKind::Roots,
            WorkoutGameTerrainKind::RockGarden,
            WorkoutGameTerrainKind::LogOver,
            WorkoutGameTerrainKind::Tabletop,
            WorkoutGameTerrainKind::Drop
        };
        for (int time = 0; time <= 12000; time += 20) {
            input.workoutTimeMs = time;
            input.terrain = terrains[(time / 2400) % 5];
            input.difficulty = 0.8;
            const WorkoutGameWorldSnapshot result = physics.update(input);
            QVERIFY2(result.rider.distanceMeters + 1e-9 >= priorDistance,
                     "forward riding published backwards course progress");
            priorDistance = result.rider.distanceMeters;
        }
        QVERIFY(priorDistance > 20.0);
    }

    void pauseAndLongUiStallDoNotRunUnboundedCatchup()
    {
        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(12u));
        WorkoutGamePhysicsInput input;
        input.desiredSpeedMetersPerSecond = 7.0;
        physics.update(input);
        input.workoutTimeMs = 1000;
        const WorkoutGameWorldSnapshot beforePause = physics.update(input);

        input.paused = true;
        input.workoutTimeMs = 60000;
        const WorkoutGameWorldSnapshot paused = physics.update(input);
        QCOMPARE(paused.rider.distanceMeters,
                 beforePause.rider.distanceMeters);

        input.paused = false;
        input.workoutTimeMs = 120000;
        const WorkoutGameWorldSnapshot resumed = physics.update(input);
        QVERIFY(resumed.rider.distanceMeters - paused.rider.distanceMeters < 15.0);
    }

    void weakClimbTransitionsToWalkingAndRecovers()
    {
        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(19u));
        WorkoutGamePhysicsInput input;
        input.terrain = WorkoutGameTerrainKind::Climb;
        input.gradePercent = 8.0;
        input.difficulty = 0.8;
        input.desiredSpeedMetersPerSecond = 4.0;
        input.effortRatio = 0.3;
        physics.update(input);
        for (int time = 100; time <= 2500; time += 100) {
            input.workoutTimeMs = time;
            physics.update(input);
        }
        QVERIFY(physics.update(input).rider.walking);

        input.effortRatio = 1.0;
        input.workoutTimeMs = 2600;
        QVERIFY(!physics.update(input).rider.walking);
    }

    void invalidTelemetryCannotCreateInvalidPose()
    {
        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(91u));
        WorkoutGamePhysicsInput input;
        input.desiredSpeedMetersPerSecond =
                std::numeric_limits<double>::quiet_NaN();
        input.gradePercent = std::numeric_limits<double>::infinity();
        input.difficulty = -std::numeric_limits<double>::infinity();
        input.effortRatio = std::numeric_limits<double>::quiet_NaN();
        physics.update(input);
        input.workoutTimeMs = 1000;

        WorkoutGameWorldSnapshot result = physics.update(input);
        QVERIFY(result.ready);
        QVERIFY(std::isfinite(result.rider.distanceMeters));
        QVERIFY(std::isfinite(result.rider.elevationMeters));
        QVERIFY(std::isfinite(result.rider.pitchDegrees));
        QVERIFY(std::isfinite(result.speedMetersPerSecond));
        QVERIFY(result.rider.rearSuspension >= 0.0);
        QVERIFY(result.rider.rearSuspension <= 1.0);

        input.workoutTimeMs = std::numeric_limits<std::int64_t>::max();
        result = physics.update(input);
        QVERIFY(std::isfinite(result.rider.distanceMeters));
        QVERIFY(std::isfinite(result.rider.elevationMeters));
    }

    void backwardsTimeStartsANewPhysicsGeneration()
    {
        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(5u));
        WorkoutGamePhysicsInput input;
        input.desiredSpeedMetersPerSecond = 5.0;
        physics.update(input);
        input.workoutTimeMs = 1000;
        const WorkoutGameWorldSnapshot before = physics.update(input);
        input.workoutTimeMs = 100;
        const WorkoutGameWorldSnapshot reset = physics.update(input);

        QVERIFY(reset.generation > before.generation);
        QVERIFY(reset.rider.distanceMeters < before.rider.distanceMeters);
    }

    void rootsExerciseSuspensionTravel()
    {
        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(997u));
        WorkoutGamePhysicsInput input;
        input.terrain = WorkoutGameTerrainKind::Roots;
        input.difficulty = 1.0;
        input.desiredSpeedMetersPerSecond = 6.0;
        input.effortRatio = 1.0;
        physics.update(input);

        double minimum = 1.0;
        double maximum = 0.0;
        for (int time = 20; time <= 5000; time += 20) {
            input.workoutTimeMs = time;
            const WorkoutGameWorldSnapshot result = physics.update(input);
            minimum = std::min(minimum, result.rider.frontSuspension);
            maximum = std::max(maximum, result.rider.frontSuspension);
        }
        QVERIFY(maximum - minimum > 0.08);
    }

    void productionRootsStayGroundedAndExerciseSuspensionAtRideSpeeds()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 713u;
        course.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::Trail;
        section.terrain = WorkoutGameTerrainKind::Roots;
        section.durationMs = course.durationMs;
        section.lengthMeters = 70.0;
        section.targetWatts = 180.0;
        section.difficulty = 0.65;
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
        const WorkoutGameRootGeometryProfile roots =
                WorkoutGameRootGeometry::profile(piece->difficulty);
        const double start = piece->challenge.obstacleDistanceMeters
                + roots.activeStartMeters - 1.0;
        const double end = piece->challenge.obstacleDistanceMeters
                + roots.activeEndMeters + 1.0;

        for (const double speed : {3.0, 5.0, 7.0}) {
            WorkoutGamePhysics physics;
            QVERIFY(physics.configure(road));
            WorkoutGamePhysicsInput input;
            input.terrain = WorkoutGameTerrainKind::Roots;
            input.desiredSpeedMetersPerSecond = speed;
            input.effortRatio = 1.0;
            double minimumSuspension = 1.0;
            double maximumSuspension = 0.0;
            int airborneTicks = 0;
            const int ticks = int(std::ceil((end - start) / (speed * 0.02)));
            for (int tick = 0; tick <= ticks; ++tick) {
                input.workoutTimeMs = tick * 20;
                input.courseDistanceMeters = std::min(
                        end, start + tick * speed * 0.02);
                const WorkoutGameWorldSnapshot result = physics.update(input);
                QVERIFY(result.ready);
                if (result.rider.airborne) ++airborneTicks;
                const double suspension = 0.5 * (
                        result.rider.rearSuspension
                        + result.rider.frontSuspension);
                minimumSuspension = std::min(minimumSuspension, suspension);
                maximumSuspension = std::max(maximumSuspension, suspension);
            }
            QVERIFY2(airborneTicks == 0,
                     qPrintable(QStringLiteral(
                         "roots became airborne at %1 m/s")
                         .arg(speed)));
            QVERIFY(maximumSuspension - minimumSuspension > 0.03);
        }
    }

    void productionRockGardenUsesBoundedMainAndSafeSuspension()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 977u;
        course.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::Trail;
        section.terrain = WorkoutGameTerrainKind::RockGarden;
        section.durationMs = course.durationMs;
        section.lengthMeters = 76.0;
        section.targetWatts = 185.0;
        section.difficulty = 0.65;
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
        const WorkoutGameRockGardenGeometryProfile rocks =
                WorkoutGameRockGardenGeometry::profile(piece->difficulty);
        const double start = piece->challenge.obstacleDistanceMeters
                + rocks.startMeters;
        const double end = piece->challenge.obstacleDistanceMeters
                + rocks.activeEndMeters + 1.0;

        for (const double speed : {3.0, 5.0, 7.0}) {
            double suspensionRanges[2] = {};
            for (int route = 0; route < 2; ++route) {
                WorkoutGamePhysics physics;
                QVERIFY(physics.configure(road));
                WorkoutGamePhysicsInput input;
                input.terrain = WorkoutGameTerrainKind::RockGarden;
                input.desiredSpeedMetersPerSecond = speed;
                input.effortRatio = route == 0 ? 1.0 : 0.75;
                input.forceGroundFollowing = route == 1;
                double minimumSuspension = 1.0;
                double maximumSuspension = 0.0;
                double maximumAirHeight = 0.0;
                const int ticks = int(std::ceil(
                        (end - start) / (speed * 0.02)));
                for (int tick = 0; tick <= ticks; ++tick) {
                    input.workoutTimeMs = tick * 20;
                    input.courseDistanceMeters = std::min(
                            end, start + tick * speed * 0.02);
                    const WorkoutGameWorldSnapshot result =
                            physics.update(input);
                    QVERIFY(result.ready);
                    QVERIFY(!result.rider.airborne);
                    maximumAirHeight = std::max(
                            maximumAirHeight,
                            result.rider.airHeightMeters());
                    const double suspension = 0.5 * (
                            result.rider.rearSuspension
                            + result.rider.frontSuspension);
                    if (input.courseDistanceMeters
                                >= piece->challenge.obstacleDistanceMeters
                                    + rocks.activeStartMeters
                            && input.courseDistanceMeters
                                <= piece->challenge.obstacleDistanceMeters
                                    + rocks.activeEndMeters) {
                        minimumSuspension = std::min(
                                minimumSuspension, suspension);
                        maximumSuspension = std::max(
                                maximumSuspension, suspension);
                    }
                }
                QVERIFY(maximumAirHeight <= 0.05);
                suspensionRanges[route] =
                        maximumSuspension - minimumSuspension;
            }
            QVERIFY2(suspensionRanges[0] > 0.05,
                     qPrintable(QStringLiteral(
                         "rock suspension range %1 at %2 m/s")
                         .arg(suspensionRanges[0]).arg(speed)));
            QVERIFY2(suspensionRanges[1] <= suspensionRanges[0] * 0.40,
                     qPrintable(QStringLiteral(
                         "safe suspension %1 exceeded 40 percent of main %2 "
                         "at %3 m/s")
                         .arg(suspensionRanges[1])
                         .arg(suspensionRanges[0])
                         .arg(speed)));
        }
    }

    void jumpableFeaturesLeaveGroundAndLand_data()
    {
        QTest::addColumn<int>("terrain");
        QTest::newRow("bunny-hop")
                << int(WorkoutGameTerrainKind::BunnyHop);
        QTest::newRow("log-over")
                << int(WorkoutGameTerrainKind::LogOver);
        QTest::newRow("tabletop")
                << int(WorkoutGameTerrainKind::Tabletop);
    }

    void jumpableFeaturesLeaveGroundAndLand()
    {
        QFETCH(int, terrain);
        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(997u));
        WorkoutGamePhysicsInput input;
        input.terrain = WorkoutGameTerrainKind(terrain);
        input.desiredSpeedMetersPerSecond = 6.0;
        input.effortRatio = 1.0;
        input.jumpRequested = true;
        input.featureActionId = 42u;
        physics.update(input);

        bool sawAirborne = false;
        bool landed = false;
        double maximumAirHeightMeters = 0.0;
        for (int time = 20; time <= 10000; time += 20) {
            input.workoutTimeMs = time;
            const WorkoutGameWorldSnapshot result = physics.update(input);
            sawAirborne = sawAirborne || result.rider.airborne;
            landed = landed || (sawAirborne && !result.rider.airborne);
            maximumAirHeightMeters = std::max(
                    maximumAirHeightMeters,
                    result.rider.airHeightMeters());
        }
        QVERIFY(sawAirborne);
        QVERIFY(landed);
        const double minimumVisibleAirHeight =
                WorkoutGameTerrainKind(terrain)
                        == WorkoutGameTerrainKind::Tabletop ? 0.32 : 0.20;
        QVERIFY2(maximumAirHeightMeters >= minimumVisibleAirHeight,
                 qPrintable(QStringLiteral(
                     "feature reached only %1 m air height")
                     .arg(maximumAirHeightMeters)));
    }

    void bunnyHopPhysicsIsShorterAndLowerThanLogOver()
    {
        struct Flight {
            bool landed = false;
            double maximumAirMeters = 0.0;
            int durationMs = 0;
        };
        const auto measure = [](WorkoutGameTerrainKind terrain) {
            WorkoutGamePhysics physics;
            Flight flight;
            if (!physics.configure(997u)) return flight;
            WorkoutGamePhysicsInput input;
            input.terrain = terrain;
            input.desiredSpeedMetersPerSecond = 6.0;
            input.effortRatio = 1.0;
            input.jumpRequested = true;
            input.featureActionId = 42u;
            physics.update(input);
            int takeoffMs = -1;
            for (int time = 20; time <= 5000; time += 20) {
                input.workoutTimeMs = time;
                const WorkoutGameWorldSnapshot result = physics.update(input);
                flight.maximumAirMeters = std::max(
                        flight.maximumAirMeters,
                        result.rider.airHeightMeters());
                if (result.rider.airborne && takeoffMs < 0) takeoffMs = time;
                if (takeoffMs >= 0 && !result.rider.airborne) {
                    flight.landed = true;
                    flight.durationMs = time - takeoffMs;
                    break;
                }
            }
            return flight;
        };

        const Flight bunny = measure(WorkoutGameTerrainKind::BunnyHop);
        const Flight log = measure(WorkoutGameTerrainKind::LogOver);
        QVERIFY(bunny.landed);
        QVERIFY(log.landed);
        QVERIFY(bunny.maximumAirMeters >= 0.25);
        QVERIFY(bunny.maximumAirMeters <= 0.65);
        QVERIFY(bunny.durationMs <= 1200);
        QVERIFY(bunny.maximumAirMeters < log.maximumAirMeters);
        QVERIFY(bunny.durationMs < log.durationMs);
    }

    void anchoredFeatureActionJumpsImmediatelyAndOnlyOnce()
    {
        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(997u));
        WorkoutGamePhysicsInput input;
        input.terrain = WorkoutGameTerrainKind::LogOver;
        input.desiredSpeedMetersPerSecond = 3.0;
        input.effortRatio = 1.0;
        input.jumpRequested = true;
        input.featureActionId = 42u;
        physics.update(input);

        bool sawAirborne = false;
        for (int time = 20; time <= 800; time += 20) {
            input.workoutTimeMs = time;
            sawAirborne = sawAirborne || physics.update(input).rider.airborne;
        }
        QVERIFY(sawAirborne);

        input.jumpRequested = false;
        bool landed = false;
        for (int time = 820; time <= 5000; time += 20) {
            input.workoutTimeMs = time;
            landed = landed || !physics.update(input).rider.airborne;
        }
        QVERIFY(landed);

        input.jumpRequested = true;
        bool jumpedAgain = false;
        for (int time = 5020; time <= 5500; time += 20) {
            input.workoutTimeMs = time;
            jumpedAgain = jumpedAgain || physics.update(input).rider.airborne;
        }
        QVERIFY(!jumpedAgain);
    }

    void dropCreatesAirTimeAndLandingImpact()
    {
        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(997u));
        WorkoutGamePhysicsInput input;
        input.terrain = WorkoutGameTerrainKind::Drop;
        input.difficulty = 0.8;
        input.desiredSpeedMetersPerSecond = 7.0;
        input.effortRatio = 1.0;
        physics.update(input);

        bool sawAirborne = false;
        double maximumImpact = 0.0;
        for (int time = 20; time <= 12000; time += 20) {
            input.workoutTimeMs = time;
            const WorkoutGameWorldSnapshot result = physics.update(input);
            sawAirborne = sawAirborne || result.rider.airborne;
            maximumImpact = std::max(maximumImpact, result.landingImpact);
        }
        QVERIFY(sawAirborne);
        QVERIFY(maximumImpact > 0.05);
    }

    void roadCourseDropCreatesBoundedUnforcedFlight()
    {
        WorkoutGameCourse source;
        source.status = WorkoutGameCourseStatus::Ready;
        source.seed = 804u;
        source.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::RecoveryDescent;
        section.terrain = WorkoutGameTerrainKind::Drop;
        section.durationMs = source.durationMs;
        section.targetWatts = 150.0;
        section.gradePercent = -4.0;
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
        const double lip = piece->challenge.obstacleDistanceMeters;

        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(road));
        WorkoutGamePhysicsInput input;
        input.terrain = WorkoutGameTerrainKind::Drop;
        input.desiredSpeedMetersPerSecond = 5.0;
        input.effortRatio = 1.0;
        input.jumpRequested = false;
        const double start = lip - 3.0;
        int takeoffMs = -1;
        int landingMs = -1;
        double takeoffDistance = 0.0;
        double landingDistance = 0.0;
        double maximumAirMeters = 0.0;
        double maximumImpact = 0.0;
        double minimumAirbornePitch = 180.0;
        double maximumAirbornePitch = -180.0;
        double maximumElevationStep = 0.0;
        double previousElevation = 0.0;
        bool havePreviousElevation = false;
        int impactTicks = 0;
        for (int tick = 0; tick <= 300; ++tick) {
            input.workoutTimeMs = tick * 20;
            input.courseDistanceMeters = start + tick * 0.10;
            const WorkoutGameWorldSnapshot result = physics.update(input);
            QVERIFY(result.ready);
            if (result.rider.airborne && takeoffMs < 0) {
                takeoffMs = input.workoutTimeMs;
                takeoffDistance = input.courseDistanceMeters;
            }
            if (result.rider.airborne) {
                minimumAirbornePitch = std::min(
                        minimumAirbornePitch,
                        result.rider.pitchDegrees);
                maximumAirbornePitch = std::max(
                        maximumAirbornePitch,
                        result.rider.pitchDegrees);
            }
            if (havePreviousElevation) {
                maximumElevationStep = std::max(
                        maximumElevationStep,
                        std::abs(result.rider.elevationMeters
                                 - previousElevation));
            }
            previousElevation = result.rider.elevationMeters;
            havePreviousElevation = true;
            if (result.landingImpact > 0.01) ++impactTicks;
            if (takeoffMs >= 0 && !result.rider.airborne) {
                landingMs = input.workoutTimeMs;
                landingDistance = input.courseDistanceMeters;
                maximumImpact = std::max(maximumImpact, result.landingImpact);
                break;
            }
            maximumAirMeters = std::max(
                    maximumAirMeters, result.rider.airHeightMeters());
            maximumImpact = std::max(maximumImpact, result.landingImpact);
        }
        QVERIFY(takeoffMs >= 0);
        QVERIFY(landingMs > takeoffMs);
        QVERIFY2(takeoffDistance >= lip - 0.35
                        && takeoffDistance <= lip + 1.10,
                 qPrintable(QStringLiteral(
                     "drop takeoff at %1 m, lip at %2 m")
                     .arg(takeoffDistance).arg(lip)));
        QVERIFY(landingMs - takeoffMs >= 200);
        QVERIFY(landingMs - takeoffMs <= 1200);
        QVERIFY(landingDistance <= lip + 5.0);
        QVERIFY(maximumAirMeters <= 1.10);
        QVERIFY(maximumImpact >= 0.05);
        QVERIFY(maximumImpact <= 0.65);
        QVERIFY(minimumAirbornePitch >= -18.0);
        QVERIFY(maximumAirbornePitch <= 4.0);
        QVERIFY(maximumElevationStep <= 0.20);
        QVERIFY(impactTicks >= 1);
        QVERIFY(impactTicks <= 4);
    }

    void roadCourseDropBypassUsesGroundedOrdinarySurface()
    {
        WorkoutGameCourse source;
        source.status = WorkoutGameCourseStatus::Ready;
        source.seed = 804u;
        source.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::RecoveryDescent;
        section.terrain = WorkoutGameTerrainKind::Drop;
        section.durationMs = source.durationMs;
        section.targetWatts = 150.0;
        section.gradePercent = -4.0;
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
        const double lip = piece->challenge.obstacleDistanceMeters;

        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(road));
        WorkoutGamePhysicsInput input;
        input.terrain = WorkoutGameTerrainKind::Drop;
        input.desiredSpeedMetersPerSecond = 5.0;
        input.effortRatio = 0.5;
        input.forceGroundFollowing = true;
        const double start = lip - 3.0;
        double maximumAirMeters = 0.0;
        double maximumImpact = 0.0;
        for (int tick = 0; tick <= 180; ++tick) {
            input.workoutTimeMs = tick * 20;
            input.courseDistanceMeters = start + tick * 0.10;
            const WorkoutGameRoadSample roadSample =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, input.courseDistanceMeters);
            const WorkoutGameWorldSnapshot result = physics.update(input);
            QVERIFY(result.ready && roadSample.ready);
            maximumAirMeters = std::max(
                    maximumAirMeters,
                    result.rider.airborne
                        ? result.rider.airHeightMeters() : 0.0);
            maximumImpact = std::max(maximumImpact, result.landingImpact);
            const double ordinarySurface = roadSample.center.elevationMeters
                    - roadSample.surfaceOffsetMeters;
            QVERIFY2(std::abs(result.surfaceElevationMeters
                              - ordinarySurface) < 0.03,
                     qPrintable(QStringLiteral(
                         "bypass surface %1, ordinary %2 at %3 m")
                         .arg(result.surfaceElevationMeters)
                         .arg(ordinarySurface)
                         .arg(input.courseDistanceMeters)));
        }
        QVERIFY(maximumAirMeters < 0.03);
        QVERIFY(maximumImpact < 0.01);
    }

    void roadCourseRollersStayGroundedAndExerciseSuspension()
    {
        WorkoutGameCourse source;
        source.status = WorkoutGameCourseStatus::Ready;
        source.seed = 905u;
        source.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::Trail;
        section.terrain = WorkoutGameTerrainKind::Rollers;
        section.durationMs = source.durationMs;
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

        const double start = piece->challenge.obstacleDistanceMeters - 5.0;
        for (const double speed : {3.33, 5.0, 7.0}) {
            WorkoutGamePhysics physics;
            QVERIFY(physics.configure(road));
            WorkoutGamePhysicsInput input;
            input.terrain = WorkoutGameTerrainKind::Rollers;
            input.desiredSpeedMetersPerSecond = speed;
            input.effortRatio = 1.0;
            double minimumSuspension = 1.0;
            double maximumSuspension = 0.0;
            double minimumPitch = 180.0;
            double maximumPitch = -180.0;
            int airborneTicks = 0;
            const int ticks = int(std::ceil(11.0 / (speed * 0.02)));
            for (int tick = 0; tick <= ticks; ++tick) {
                input.workoutTimeMs = tick * 20;
                input.courseDistanceMeters = start + tick * speed * 0.02;
                const WorkoutGameWorldSnapshot result = physics.update(input);
                QVERIFY(result.ready);
                if (result.rider.airborne) ++airborneTicks;
                const double suspension = 0.5 * (
                        result.rider.rearSuspension
                        + result.rider.frontSuspension);
                minimumSuspension = std::min(minimumSuspension, suspension);
                maximumSuspension = std::max(maximumSuspension, suspension);
                minimumPitch = std::min(
                        minimumPitch, result.rider.pitchDegrees);
                maximumPitch = std::max(
                        maximumPitch, result.rider.pitchDegrees);
            }
            QVERIFY2(airborneTicks == 0,
                     qPrintable(QStringLiteral(
                         "rollers at %1 m/s: %2 airborne ticks, suspension "
                         "%3..%4, pitch %5..%6")
                         .arg(speed).arg(airborneTicks)
                         .arg(minimumSuspension).arg(maximumSuspension)
                         .arg(minimumPitch).arg(maximumPitch)));
            QVERIFY(maximumSuspension - minimumSuspension >= 0.08);
            QVERIFY(minimumPitch < -1.0);
            QVERIFY(maximumPitch > 1.0);
        }
    }

    void longRunRebasesWithoutFallingOffWorld()
    {
        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(997u));
        WorkoutGamePhysicsInput input;
        input.terrain = WorkoutGameTerrainKind::Rollers;
        input.difficulty = 0.5;
        input.desiredSpeedMetersPerSecond = 8.0;
        input.effortRatio = 1.0;
        WorkoutGameWorldSnapshot prior = physics.update(input);

        for (int time = 100; time <= 90000; time += 100) {
            input.workoutTimeMs = time;
            const WorkoutGameWorldSnapshot result = physics.update(input);
            QVERIFY(result.rider.distanceMeters >= prior.rider.distanceMeters - 0.1);
            QVERIFY(result.rider.distanceMeters - prior.rider.distanceMeters < 2.0);
            QVERIFY(std::isfinite(result.rider.elevationMeters));
            QVERIFY(std::abs(result.rider.elevationMeters) < 20.0);
            prior = result;
        }
        QVERIFY(prior.rider.distanceMeters > 400.0);
    }

    void cameraModesMatchFeatureGeometry()
    {
        for (WorkoutGameTerrainKind terrain : {
                WorkoutGameTerrainKind::SmoothTrail,
                WorkoutGameTerrainKind::Roots,
                WorkoutGameTerrainKind::Rollers,
                WorkoutGameTerrainKind::Climb,
                WorkoutGameTerrainKind::RockGarden,
                WorkoutGameTerrainKind::BunnyHop,
                WorkoutGameTerrainKind::Drop,
                WorkoutGameTerrainKind::Skinny,
                WorkoutGameTerrainKind::Berm,
                WorkoutGameTerrainKind::LogOver,
                WorkoutGameTerrainKind::Tabletop,
                WorkoutGameTerrainKind::RockSlab}) {
            QCOMPARE(WorkoutGameCamera::preferredMode(terrain),
                     WorkoutGameCameraMode::ThreeQuarter);
        }
    }

    void bermRemainsGroundedAtRepresentativeRidingSpeeds()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 407u;
        course.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::Trail;
        section.terrain = WorkoutGameTerrainKind::Berm;
        section.durationMs = course.durationMs;
        section.lengthMeters = 70.0;
        section.targetWatts = 180.0;
        section.difficulty = 0.65;
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
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(piece->difficulty);

        for (double speed : {3.0, 5.0, 7.0}) {
            WorkoutGamePhysics physics;
            QVERIFY(physics.configure(road));
            WorkoutGamePhysicsInput input;
            input.terrain = WorkoutGameTerrainKind::Berm;
            input.desiredSpeedMetersPerSecond = speed;
            input.effortRatio = 1.0;
            const double start = piece->challenge.obstacleDistanceMeters
                    + profile.startMeters - 1.0;
            const double end = piece->challenge.obstacleDistanceMeters
                    + profile.endMeters + 1.0;
            for (int tick = 0; tick <= 160; ++tick) {
                input.workoutTimeMs = tick * 20;
                input.courseDistanceMeters = start
                        + (end - start) * double(tick) / 160.0;
                const WorkoutGameRoadSample roadSample =
                        WorkoutGameRoadCourseBuilder::sample(
                            road, input.courseDistanceMeters);
                input.gradePercent = roadSample.center.gradePercent;
                const WorkoutGameWorldSnapshot frame = physics.update(input);
                QVERIFY(frame.ready);
                QVERIFY2(!frame.rider.airborne,
                         qPrintable(QStringLiteral(
                             "berm became airborne at %1 m/s and %2 m")
                             .arg(speed).arg(input.courseDistanceMeters)));
            }
        }
    }

    void addedFeaturesHaveDistinctRideableProfiles()
    {
        const double flat = WorkoutGamePhysics::terrainHeight(
                WorkoutGameTerrainKind::SmoothTrail, 30.0, 0.0, 0.7, 0u);
        const double log = WorkoutGamePhysics::terrainHeight(
                WorkoutGameTerrainKind::LogOver, 30.0, 0.0, 0.7, 0u);
        const double tabletop = WorkoutGamePhysics::terrainHeight(
                WorkoutGameTerrainKind::Tabletop, 30.0, 0.0, 0.7, 0u);
        const double slab = WorkoutGamePhysics::terrainHeight(
                WorkoutGameTerrainKind::RockSlab, 30.0, 0.0, 0.7, 0u);

        QVERIFY(log > flat + 0.15);
        QVERIFY(tabletop > flat + 0.3);
        QVERIFY(slab > flat + 0.1);
    }

    void initialSnapshotFramesTheRider()
    {
        WorkoutGameWorldSnapshot world;
        world.ready = true;
        world.generation = 4;
        world.terrain = WorkoutGameTerrainKind::Roots;
        world.rider.distanceMeters = 25.0;
        world.rider.elevationMeters = 3.0;
        world.speedMetersPerSecond = 8.0;

        WorkoutGameCamera camera;
        const WorkoutGameCameraSnapshot view = camera.update(world, 0.016);

        QVERIFY(view.ready);
        QCOMPARE(view.mode, WorkoutGameCameraMode::ThreeQuarter);
        QCOMPARE(view.centerDistanceMeters, 25.0);
        QCOMPARE(view.centerElevationMeters, 4.2);
        QCOMPARE(view.yawDegrees, 42.0);
        QVERIFY(view.lookAheadMeters > 3.0);
    }

    void cameraTracksTrailSurfaceInsteadOfAirborneChassis()
    {
        WorkoutGameWorldSnapshot world;
        world.ready = true;
        world.generation = 1;
        world.rider.distanceMeters = 25.0;
        world.rider.elevationMeters = 5.0;
        world.rider.clearanceMeters = 1.82;
        world.rider.airborne = true;

        QCOMPARE(world.rider.airHeightMeters(), 1.0);
        WorkoutGameCamera camera;
        const WorkoutGameCameraSnapshot view = camera.update(world, 0.016);
        QCOMPARE(view.centerElevationMeters, 5.2);

        world.rider.airborne = false;
        QCOMPARE(world.rider.airHeightMeters(), 0.0);
        world.rider.clearanceMeters = 0.90;
        QCOMPARE(world.rider.airHeightMeters(), 0.0);
        world.rider.airborne = true;
        QVERIFY(std::abs(world.rider.airHeightMeters() - 0.08) < 1e-9);
    }

    void fixedObliqueCameraKeepsSmoothFeatureZoom()
    {
        WorkoutGameWorldSnapshot world;
        world.ready = true;
        world.generation = 1;
        world.terrain = WorkoutGameTerrainKind::SmoothTrail;
        world.rider.distanceMeters = 10.0;
        world.speedMetersPerSecond = 7.0;

        WorkoutGameCamera camera;
        const WorkoutGameCameraSnapshot initial = camera.update(world, 0.016);
        QCOMPARE(initial.yawDegrees, 42.0);
        QCOMPARE(initial.zoom, 1.0);
        world.terrain = WorkoutGameTerrainKind::Skinny;
        world.rider.distanceMeters = 15.0;

        const WorkoutGameCameraSnapshot first = camera.update(world, 0.016);
        QCOMPARE(first.yawDegrees, 42.0);
        QVERIFY(first.zoom > 1.0);
        QVERIFY(first.zoom < 1.08);
        QVERIFY(first.centerDistanceMeters > 10.0);
        QVERIFY(first.centerDistanceMeters < 15.0);

        WorkoutGameCameraSnapshot settled = first;
        for (int frame = 0; frame < 180; ++frame) {
            settled = camera.update(world, 1.0 / 60.0);
        }
        QCOMPARE(settled.yawDegrees, 42.0);
        QVERIFY(std::abs(settled.zoom - 1.08) < 0.002);
        QVERIFY(std::abs(settled.centerDistanceMeters - 15.0) < 0.01);
    }

    void generationChangeCutsToNewWorldSafely()
    {
        WorkoutGameWorldSnapshot world;
        world.ready = true;
        world.generation = 1;
        world.rider.distanceMeters = 80.0;

        WorkoutGameCamera camera;
        camera.update(world, 0.016);
        world.generation = 2;
        world.rider.distanceMeters = 0.0;
        world.rider.elevationMeters = 5.0;

        const WorkoutGameCameraSnapshot reset = camera.update(world, 0.016);
        QCOMPARE(reset.centerDistanceMeters, 0.0);
        QCOMPARE(reset.centerElevationMeters, 6.2);
    }

    void terrainGenerationChangeKeepsCameraContinuous()
    {
        WorkoutGameWorldSnapshot world;
        world.ready = true;
        world.generation = 1;
        world.terrain = WorkoutGameTerrainKind::SmoothTrail;
        world.rider.distanceMeters = 80.0;

        WorkoutGameCamera camera;
        QCOMPARE(camera.update(world, 0.016).yawDegrees, 42.0);

        world.generation = 2;
        world.terrain = WorkoutGameTerrainKind::RockGarden;
        world.rider.distanceMeters = 80.5;
        const WorkoutGameCameraSnapshot transition =
                camera.update(world, 0.016);

        QVERIFY(transition.centerDistanceMeters > 80.0);
        QVERIFY(transition.centerDistanceMeters < 80.5);
        QCOMPARE(transition.yawDegrees, 42.0);
    }

    void invalidInputsCannotPoisonCameraState()
    {
        WorkoutGameWorldSnapshot world;
        world.ready = true;
        world.generation = 1;
        world.rider.distanceMeters = std::numeric_limits<double>::quiet_NaN();
        world.rider.elevationMeters = std::numeric_limits<double>::infinity();
        world.rider.pitchDegrees = -std::numeric_limits<double>::infinity();
        world.speedMetersPerSecond = std::numeric_limits<double>::quiet_NaN();

        WorkoutGameCamera camera;
        const WorkoutGameCameraSnapshot view = camera.update(world, 10.0);

        QVERIFY(std::isfinite(view.centerDistanceMeters));
        QVERIFY(std::isfinite(view.centerElevationMeters));
        QVERIFY(std::isfinite(view.lookAheadMeters));
        QVERIFY(std::isfinite(view.yawDegrees));
        QVERIFY(std::isfinite(view.pitchDegrees));
    }

    void notReadyWorldResetsCamera()
    {
        WorkoutGameWorldSnapshot world;
        world.ready = true;
        world.generation = 1;
        WorkoutGameCamera camera;
        QVERIFY(camera.update(world, 0.016).ready);

        world.ready = false;
        QVERIFY(!camera.update(world, 0.016).ready);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameWorld)
#include "testWorkoutGameWorld.moc"
