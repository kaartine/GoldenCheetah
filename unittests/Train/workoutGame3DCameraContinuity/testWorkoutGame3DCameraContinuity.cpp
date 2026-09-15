/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DViewModel.h"
#include "WorkoutGameRoadCourse.h"

#include <QTest>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr double FtpWatts = 200.0;
constexpr double Pi = 3.14159265358979323846;

WorkoutGameCourse cameraMotionCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 0x334456u;
    course.durationMs = 100000;
    const std::array<WorkoutGameTerrainKind, 5> terrains = {{
        WorkoutGameTerrainKind::Climb,
        WorkoutGameTerrainKind::Roots,
        WorkoutGameTerrainKind::Tabletop,
        WorkoutGameTerrainKind::RockGarden,
        WorkoutGameTerrainKind::Drop
    }};
    const std::array<double, 5> lengths = {{18.0, 16.0, 28.0, 16.0, 24.0}};
    std::int64_t startMs = 0;
    for (std::size_t index = 0; index < terrains.size(); ++index) {
        WorkoutGameSection section;
        section.feature = index == 2
                ? WorkoutGameFeature::SprintJump
                : WorkoutGameFeature::Trail;
        section.terrain = terrains[index];
        section.startMs = startMs;
        section.durationMs = 20000;
        section.targetWatts = 175.0 + double(index) * 22.0;
        section.gradePercent = index == 0 ? 8.0 : (index == 4 ? -7.0 : 2.0);
        section.lengthMeters = lengths[index];
        section.difficulty = 0.35 + double(index) * 0.1;
        section.challengeCount = 1;
        section.visualVariant = std::uint32_t(index + 1);
        section.gravityAssisted = index == 4;
        course.sections.push_back(section);
        startMs += section.durationMs;
    }
    return course;
}

WorkoutGameCourse rootBedCameraCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 0x600d5u;
    course.durationMs = 60000;
    WorkoutGameSection roots;
    roots.feature = WorkoutGameFeature::Trail;
    roots.terrain = WorkoutGameTerrainKind::Roots;
    roots.durationMs = course.durationMs;
    roots.targetWatts = 210.0;
    roots.gradePercent = 0.0;
    roots.lengthMeters = 80.0;
    roots.difficulty = 1.0;
    roots.challengeCount = 1;
    course.sections.push_back(roots);
    return course;
}

WorkoutGameVisualSnapshot frameAt(
        const WorkoutGameRoadCourse &road,
        double distanceMeters,
        std::int64_t timeMs,
        double speedKph)
{
    const WorkoutGameRoadSample sample =
            WorkoutGameRoadCourseBuilder::sample(road, distanceMeters);
    WorkoutGameVisualSnapshot frame;
    frame.world.ready = true;
    frame.world.terrain = sample.terrain;
    frame.world.gradePercent = sample.center.gradePercent;
    frame.world.rider.distanceMeters = distanceMeters;
    frame.world.rider.elevationMeters = sample.center.elevationMeters;
    frame.world.rider.pitchDegrees = sample.center.gradePercent * 0.45;
    frame.simulation.ready = true;
    frame.simulation.workoutTimeMs = timeMs;
    frame.simulation.speedKph = speedKph;
    frame.presentationTimeMs = timeMs;
    frame.riderPedalCycles = distanceMeters * 0.35;
    return frame;
}

double normalizedRadians(double angle)
{
    return std::remainder(angle, 2.0 * Pi);
}

struct CameraMeasurement
{
    double y = 0.0;
    double yaw = 0.0;
    double pitch = 0.0;
    double riderYawError = 0.0;
    double riderPitchError = 0.0;
};

CameraMeasurement measure(const WorkoutGame3DViewModel &model)
{
    const double viewX = model.cameraTargetX() - model.cameraX();
    const double viewY = model.cameraTargetY() - model.cameraY();
    const double viewZ = model.cameraTargetZ() - model.cameraZ();
    const double riderX = model.riderX() - model.cameraX();
    const double riderY = model.riderY() + 0.9 - model.cameraY();
    const double riderZ = model.riderZ() - model.cameraZ();
    const double viewHorizontal = std::hypot(viewX, viewZ);
    const double riderHorizontal = std::hypot(riderX, riderZ);
    CameraMeasurement result;
    result.y = model.cameraY();
    result.yaw = std::atan2(viewX, viewZ);
    result.pitch = std::atan2(viewY, viewHorizontal);
    result.riderYawError = normalizedRadians(
            std::atan2(riderX, riderZ) - result.yaw);
    result.riderPitchError = std::atan2(riderY, riderHorizontal)
            - result.pitch;
    return result;
}

}

class TestWorkoutGame3DCameraContinuity : public QObject
{
    Q_OBJECT

private slots:
    void adjacentFramesRemainContinuous_data()
    {
        QTest::addColumn<int>("frameRate");
        QTest::newRow("30-fps") << 30;
        QTest::newRow("60-fps") << 60;
    }

    void adjacentFramesRemainContinuous()
    {
        QFETCH(int, frameRate);
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        constexpr double DurationSeconds = 12.0;
        const int frameCount = int(DurationSeconds * frameRate) + 1;
        const double startDistance = 2.0;
        const double endDistance = road.totalLengthMeters - 2.0;
        const double speedKph = (endDistance - startDistance)
                / DurationSeconds * 3.6;
        WorkoutGame3DViewModel model;
        model.setCourse(course, FtpWatts);

        CameraMeasurement previous;
        bool havePrevious = false;
        double maximumVerticalSpeed = 0.0;
        double maximumPitchSpeed = 0.0;
        double maximumYawSpeed = 0.0;
        double maximumRiderYawError = 0.0;
        double maximumRiderPitchError = 0.0;
        for (int index = 0; index < frameCount; ++index) {
            const double elapsedSeconds = double(index) / frameRate;
            const double progress = elapsedSeconds / DurationSeconds;
            const double distance = startDistance
                    + (endDistance - startDistance) * progress;
            const std::int64_t timeMs = std::int64_t(std::llround(
                    elapsedSeconds * 1000.0));
            model.setFrame(
                    frameAt(road, distance, timeMs, speedKph),
                    220.0, 220.0, 88, 150, 7);
            const CameraMeasurement current = measure(model);
            maximumRiderYawError = std::max(
                    maximumRiderYawError, std::abs(current.riderYawError));
            maximumRiderPitchError = std::max(
                    maximumRiderPitchError, std::abs(current.riderPitchError));
            if (havePrevious) {
                const double deltaSeconds = 1.0 / frameRate;
                maximumVerticalSpeed = std::max(
                        maximumVerticalSpeed,
                        std::abs(current.y - previous.y) / deltaSeconds);
                maximumPitchSpeed = std::max(
                        maximumPitchSpeed,
                        std::abs(current.pitch - previous.pitch)
                            / deltaSeconds);
                maximumYawSpeed = std::max(
                        maximumYawSpeed,
                        std::abs(normalizedRadians(
                            current.yaw - previous.yaw)) / deltaSeconds);
            }
            previous = current;
            havePrevious = true;
        }

        qInfo("%d FPS: vertical %.3f m/s, pitch %.3f rad/s, yaw %.3f "
              "rad/s, rider errors %.3f/%.3f rad",
              frameRate, maximumVerticalSpeed, maximumPitchSpeed,
              maximumYawSpeed, maximumRiderYawError,
              maximumRiderPitchError);
        QVERIFY2(maximumVerticalSpeed <= 6.0,
                 "camera height has an adjacent-frame discontinuity");
        QVERIFY2(maximumPitchSpeed <= 0.90,
                 "camera pitch has an adjacent-frame discontinuity");
        QVERIFY2(maximumYawSpeed <= 1.30,
                 "camera yaw has an adjacent-frame discontinuity");
        QVERIFY2(maximumRiderYawError <= 14.0 * Pi / 180.0,
                 "rider left the horizontal camera safe area");
        QVERIFY2(maximumRiderPitchError <= 14.0 * Pi / 180.0,
                 "rider left the vertical camera safe area");
    }

    void rootsKeepChaseCameraComfortable_data()
    {
        QTest::addColumn<int>("frameRate");
        QTest::newRow("30-fps") << 30;
        QTest::newRow("60-fps") << 60;
    }

    void rootSurfaceDoesNotBecomeCameraSupport()
    {
        WorkoutGameRoadSample sample;
        sample.ready = true;
        sample.terrain = WorkoutGameTerrainKind::Roots;
        sample.center.elevationMeters = 1.25;
        sample.surfaceOffsetMeters = 0.12;
        QCOMPARE(WorkoutGame3DCameraComfort::supportElevationMeters(sample),
                 1.13);

        sample.terrain = WorkoutGameTerrainKind::SmoothTrail;
        QCOMPARE(WorkoutGame3DCameraComfort::supportElevationMeters(sample),
                 1.25);
    }

    void rootCameraTextureIsSmallAndRateLimited_data()
    {
        QTest::addColumn<int>("frameRate");
        QTest::newRow("30-fps") << 30;
        QTest::newRow("60-fps") << 60;
    }

    void rootCameraTextureIsSmallAndRateLimited()
    {
        QFETCH(int, frameRate);
        WorkoutGame3DCameraComfort comfort;
        comfort.reset();
        comfort.update({0, WorkoutGameTerrainKind::Roots, true, 0.0});
        double previous = 0.0;
        double peak = 0.0;
        double maximumSpeed = 0.0;
        const int frameCount = frameRate;
        std::int64_t previousTimeMs = 0;
        for (int index = 1; index <= frameCount; ++index) {
            const std::int64_t timeMs = std::int64_t(std::llround(
                    double(index) * 1000.0 / frameRate));
            const double timeSeconds = double(timeMs) / 1000.0;
            const double rootHeight = timeSeconds >= 0.20
                    && timeSeconds <= 0.55 ? 0.13 : 0.0;
            const double current = comfort.update({
                timeMs, WorkoutGameTerrainKind::Roots, true, rootHeight
            });
            const double elapsedSeconds =
                    double(timeMs - previousTimeMs) / 1000.0;
            peak = std::max(peak, current);
            maximumSpeed = std::max(
                    maximumSpeed,
                    std::abs(current - previous) / elapsedSeconds);
            previous = current;
            previousTimeMs = timeMs;
        }

        QVERIFY(peak >= 0.010);
        QVERIFY(peak <= WorkoutGame3DCameraComfort::MaximumRootBumpMeters);
        QVERIFY(maximumSpeed
                <= WorkoutGame3DCameraComfort
                    ::MaximumVerticalSpeedMetersPerSecond + 1.0e-9);

        const double bypass = comfort.update({
            1100, WorkoutGameTerrainKind::Roots, false, 0.13
        });
        QVERIFY(bypass < previous);
    }

    void rootsKeepChaseCameraComfortable()
    {
        QFETCH(int, frameRate);
        const WorkoutGameCourse course = rootBedCameraCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto roots = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.terrain == WorkoutGameTerrainKind::Roots
                            && piece.challenge.enabled;
                });
        QVERIFY(roots != road.pieces.end());

        WorkoutGame3DViewModel model;
        model.setCourse(course, FtpWatts);
        model.setFrame(frameAt(road, 1.0, 0, 20.0),
                       210.0, 210.0, 82, 145, 6);
        model.setFrame(frameAt(road, 1.0, 5000, 20.0),
                       210.0, 210.0, 82, 145, 6);

        const double start = std::max(
                2.0, roots->challenge.obstacleDistanceMeters - 3.0);
        const double end = std::min(
                road.totalLengthMeters - 2.0,
                roots->challenge.obstacleDistanceMeters + 12.0);
        constexpr double SpeedMetersPerSecond = 20.0 / 3.6;
        const int frameCount = int(std::ceil(
                (end - start) / SpeedMetersPerSecond * frameRate));
        double maximumCameraLift = 0.0;
        double maximumVerticalSpeed = 0.0;
        double maximumComfortOffset = 0.0;
        double previousY = model.cameraY();
        for (int index = 0; index <= frameCount; ++index) {
            const double progress = frameCount > 0
                    ? double(index) / frameCount : 1.0;
            const double distance = start + (end - start) * progress;
            const std::int64_t timeMs = 5100 + std::int64_t(std::llround(
                    (distance - start) / SpeedMetersPerSecond * 1000.0));
            WorkoutGameVisualSnapshot frame =
                    frameAt(road, distance, timeMs, 20.0);
            frame.feature.route = WorkoutGameRoute::MainLine;
            model.setFrame(frame, 210.0, 210.0, 82, 145, 6);

            const double baseCameraDistance = std::max(
                    0.0, distance - model.cameraBackMeters());
            const WorkoutGameRoadSample cameraRoad =
                    WorkoutGameRoadCourseBuilder::sampleVisual(
                        road, baseCameraDistance);
            QVERIFY(cameraRoad.ready);
            const double smoothGround = cameraRoad.visualGroundElevationMeters()
                    - (cameraRoad.terrain == WorkoutGameTerrainKind::Roots
                        ? cameraRoad.surfaceOffsetMeters : 0.0);
            maximumCameraLift = std::max(
                    maximumCameraLift,
                    model.cameraY()
                        - (smoothGround + model.cameraHeightMeters()));
            maximumComfortOffset = std::max(
                    maximumComfortOffset, model.cameraComfortOffset());
            if (index > 0) {
                maximumVerticalSpeed = std::max(
                        maximumVerticalSpeed,
                        std::abs(model.cameraY() - previousY) * frameRate);
            }
            previousY = model.cameraY();
        }

        qInfo("root bed %d FPS: camera lift %.3f m, vertical %.3f m/s, "
              "texture %.3f m", frameRate, maximumCameraLift,
              maximumVerticalSpeed, maximumComfortOffset);
        QVERIFY2(maximumCameraLift <= 0.035,
                 "roots lifted the chase camera too far");
        QVERIFY(maximumComfortOffset
                <= WorkoutGame3DCameraComfort::MaximumRootBumpMeters);
    }

    void sparseSideToChaseUpdateKeepsRiderInSafeArea()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel model;
        model.setCourse(course, FtpWatts);

        model.setFrame(frameAt(road, 18.0, 0, 22.5),
                       180.0, 180.0, 85, 120, 5);
        model.setFrame(frameAt(road, 18.0, 3000, 22.5),
                       180.0, 180.0, 85, 120, 5);
        model.setFrame(frameAt(road, 18.0, 5000, 22.5),
                       180.0, 180.0, 85, 120, 5);
        const CameraMeasurement chase = measure(model);
        qInfo("sparse side-to-chase rider errors %.3f/%.3f rad",
              std::abs(chase.riderYawError),
              std::abs(chase.riderPitchError));
        QVERIFY2(std::abs(chase.riderYawError) <= 10.0 * Pi / 180.0,
                 "side-to-chase update pushed rider out horizontally");
        QVERIFY2(std::abs(chase.riderPitchError) <= 12.0 * Pi / 180.0,
                 "side-to-chase update pushed rider out vertically");
    }

    void steepClimbKeepsCameraAboveTheRiderGroundPlane()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 0x51ee9u;
        course.durationMs = 120000;
        WorkoutGameSection climb;
        climb.feature = WorkoutGameFeature::Climb;
        climb.terrain = WorkoutGameTerrainKind::Climb;
        climb.durationMs = course.durationMs;
        climb.targetWatts = 280.0;
        climb.gradePercent = 30.0;
        climb.lengthMeters = 80.0;
        climb.difficulty = 1.0;
        climb.challengeCount = 1;
        course.sections.push_back(climb);

        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel model;
        model.setCourse(course, FtpWatts);

        for (int index = 0; index <= 240; ++index) {
            const double distance = 10.0 + 60.0 * double(index) / 240.0;
            const std::int64_t timeMs = index * 50;
            const WorkoutGameRoadSample rider =
                    WorkoutGameRoadCourseBuilder::sample(road, distance);
            model.setFrame(frameAt(road, distance, timeMs, 18.0),
                           280.0, 280.0, 72, 155, 5);
            QVERIFY2(model.cameraY() >= rider.visualGroundElevationMeters()
                        + 1.50 - 1.0e-6,
                     "camera entered the rising terrain behind the rider");
        }
    }

    void terrainClearanceWinsWhenCameraCannotSmoothAHeightChange()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 0x51eeau;
        course.durationMs = 120000;
        WorkoutGameSection climb;
        climb.feature = WorkoutGameFeature::Climb;
        climb.terrain = WorkoutGameTerrainKind::Climb;
        climb.durationMs = course.durationMs;
        climb.targetWatts = 280.0;
        climb.gradePercent = 30.0;
        climb.lengthMeters = 80.0;
        climb.difficulty = 1.0;
        climb.challengeCount = 1;
        course.sections.push_back(climb);

        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel model;
        model.setCourse(course, FtpWatts);

        model.setFrame(frameAt(road, 2.0, 0, 18.0),
                       280.0, 280.0, 72, 155, 5);

        constexpr double riderDistanceMeters = 45.0;
        const WorkoutGameRoadSample rider =
                WorkoutGameRoadCourseBuilder::sample(
                    road, riderDistanceMeters);
        QVERIFY(rider.ready);
        model.setFrame(frameAt(road, riderDistanceMeters, 50, 18.0),
                       280.0, 280.0, 72, 155, 5);

        QVERIFY2(model.cameraY() >= rider.visualGroundElevationMeters()
                    + 1.50 - 1.0e-6,
                 "camera smoothing overrode the terrain exclusion height");
    }

    void terrainClearanceAppliesOnTheNextAdvancingTimestamp()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 0x51eebu;
        course.durationMs = 120000;
        WorkoutGameSection climb;
        climb.feature = WorkoutGameFeature::Climb;
        climb.terrain = WorkoutGameTerrainKind::Climb;
        climb.durationMs = course.durationMs;
        climb.targetWatts = 280.0;
        climb.gradePercent = 30.0;
        climb.lengthMeters = 80.0;
        climb.difficulty = 1.0;
        climb.challengeCount = 1;
        course.sections.push_back(climb);

        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel model;
        model.setCourse(course, FtpWatts);

        constexpr std::int64_t repeatedTimeMs = 1000;
        model.setFrame(frameAt(road, 2.0, repeatedTimeMs, 18.0),
                       280.0, 280.0, 72, 155, 5);

        constexpr double riderDistanceMeters = 45.0;
        const WorkoutGameRoadSample rider =
                WorkoutGameRoadCourseBuilder::sample(
                    road, riderDistanceMeters);
        QVERIFY(rider.ready);
        const double previousCameraY = model.cameraY();
        model.setFrame(
                frameAt(road, riderDistanceMeters, repeatedTimeMs, 18.0),
                280.0, 280.0, 72, 155, 5);
        QCOMPARE(model.cameraY(), previousCameraY);

        model.setFrame(
                frameAt(road, riderDistanceMeters, repeatedTimeMs + 1, 18.0),
                280.0, 280.0, 72, 155, 5);

        QVERIFY2(model.cameraY() >= rider.visualGroundElevationMeters()
                    + 1.50 - 1.0e-6,
                 "the next camera tick did not restore terrain clearance");
    }
};

QTEST_MAIN(TestWorkoutGame3DCameraContinuity)
#include "testWorkoutGame3DCameraContinuity.moc"
