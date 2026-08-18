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

#include <cmath>
#include <limits>

class TestWorkoutGameWorld : public QObject
{
    Q_OBJECT

private slots:
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
