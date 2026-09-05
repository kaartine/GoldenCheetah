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
};

QTEST_MAIN(TestWorkoutGame3DCameraContinuity)
#include "testWorkoutGame3DCameraContinuity.moc"
