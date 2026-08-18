/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameWorld.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <limits>

class TestWorkoutGameWorld : public QObject
{
    Q_OBJECT

private slots:
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

    void bunnyHopLeavesGroundAndLands()
    {
        WorkoutGamePhysics physics;
        QVERIFY(physics.configure(997u));
        WorkoutGamePhysicsInput input;
        input.terrain = WorkoutGameTerrainKind::BunnyHop;
        input.desiredSpeedMetersPerSecond = 6.0;
        input.effortRatio = 1.0;
        input.jumpRequested = true;
        physics.update(input);

        bool sawAirborne = false;
        bool landed = false;
        for (int time = 20; time <= 10000; time += 20) {
            input.workoutTimeMs = time;
            const WorkoutGameWorldSnapshot result = physics.update(input);
            sawAirborne = sawAirborne || result.rider.airborne;
            landed = landed || (sawAirborne && !result.rider.airborne);
        }
        QVERIFY(sawAirborne);
        QVERIFY(landed);
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
        QCOMPARE(WorkoutGameCamera::preferredMode(
                    WorkoutGameTerrainKind::Climb),
                 WorkoutGameCameraMode::Side);
        QCOMPARE(WorkoutGameCamera::preferredMode(
                    WorkoutGameTerrainKind::Roots),
                 WorkoutGameCameraMode::ThreeQuarter);
        QCOMPARE(WorkoutGameCamera::preferredMode(
                    WorkoutGameTerrainKind::RockGarden),
                 WorkoutGameCameraMode::ThreeQuarter);
        QCOMPARE(WorkoutGameCamera::preferredMode(
                    WorkoutGameTerrainKind::Skinny),
                 WorkoutGameCameraMode::Chase);
        QCOMPARE(WorkoutGameCamera::preferredMode(
                    WorkoutGameTerrainKind::Berm),
                 WorkoutGameCameraMode::Chase);
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

    void cameraTransitionIsSmoothAndConverges()
    {
        WorkoutGameWorldSnapshot world;
        world.ready = true;
        world.generation = 1;
        world.terrain = WorkoutGameTerrainKind::SmoothTrail;
        world.rider.distanceMeters = 10.0;
        world.speedMetersPerSecond = 7.0;

        WorkoutGameCamera camera;
        QCOMPARE(camera.update(world, 0.016).yawDegrees, 90.0);
        world.terrain = WorkoutGameTerrainKind::Skinny;
        world.rider.distanceMeters = 15.0;

        const WorkoutGameCameraSnapshot first = camera.update(world, 0.016);
        QVERIFY(first.yawDegrees < 90.0);
        QVERIFY(first.yawDegrees > 8.0);
        QVERIFY(first.centerDistanceMeters > 10.0);
        QVERIFY(first.centerDistanceMeters < 15.0);

        WorkoutGameCameraSnapshot settled = first;
        for (int frame = 0; frame < 180; ++frame) {
            settled = camera.update(world, 1.0 / 60.0);
        }
        QVERIFY(std::abs(settled.yawDegrees - 8.0) < 0.2);
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
