/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameCanvas.h"
#include "Train/WorkoutGameCompetition.h"
#include "Train/WorkoutGameOpenGLCanvas.h"
#include "Train/WorkoutGameVisualSmoother.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSet>
#include <QSignalSpy>
#include <QTest>

namespace {

QImage render(WorkoutGameCanvas &canvas)
{
    QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    canvas.render(&painter);
    return image;
}

QSet<QRgb> colors(const QImage &image)
{
    QSet<QRgb> result;
    for (int y = 0; y < image.height(); y += 4) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x += 4) result.insert(line[x]);
    }
    return result;
}

int changedPixels(const QImage &first, const QImage &second)
{
    if (first.size() != second.size()) return 0;
    int changed = 0;
    for (int y = 0; y < first.height(); y += 2) {
        for (int x = 0; x < first.width(); x += 2) {
            if (first.pixel(x, y) != second.pixel(x, y)) ++changed;
        }
    }
    return changed;
}

}

class TestWorkoutGameCanvas : public QObject
{
    Q_OBJECT

private slots:
    void visualStateInterpolatesBetweenTelemetryUpdates()
    {
        WorkoutGameVisualSnapshot first;
        first.simulation.ready = true;
        first.simulation.activeSection = 2;
        first.simulation.courseProgress = 0.2;
        first.simulation.speedKph = 20.0;
        first.world.ready = true;
        first.world.generation = 4;
        first.world.rider.distanceMeters = 10.0;
        first.world.rider.pitchDegrees = 350.0;
        first.camera.ready = true;
        first.camera.centerDistanceMeters = 9.0;

        WorkoutGameVisualSnapshot second = first;
        second.simulation.courseProgress = 0.4;
        second.simulation.speedKph = 30.0;
        second.world.rider.distanceMeters = 14.0;
        second.world.rider.pitchDegrees = 10.0;
        second.camera.centerDistanceMeters = 13.0;

        WorkoutGameVisualSmoother smoother;
        smoother.setTarget(first, 1000);
        smoother.setTarget(second, 1200);
        const WorkoutGameVisualSnapshot halfway = smoother.sample(1300);

        QVERIFY(std::abs(halfway.simulation.courseProgress - 0.3) < 1e-9);
        QVERIFY(std::abs(halfway.simulation.speedKph - 25.0) < 1e-9);
        QVERIFY(std::abs(halfway.world.rider.distanceMeters - 12.0) < 1e-9);
        QVERIFY(std::abs(halfway.world.rider.pitchDegrees - 360.0) < 1e-9);
        QVERIFY(std::abs(halfway.camera.centerDistanceMeters - 11.0) < 1e-9);
        QCOMPARE(smoother.sample(1400).world.rider.distanceMeters, 14.0);
    }

    void visualStateDoesNotBlendAcrossCourseSections()
    {
        WorkoutGameVisualSnapshot first;
        first.simulation.ready = true;
        first.simulation.activeSection = 1;
        first.world.ready = true;
        first.world.generation = 3;
        first.world.rider.distanceMeters = 20.0;
        first.camera.ready = true;

        WorkoutGameVisualSnapshot second = first;
        second.simulation.activeSection = 2;
        second.world.generation = 4;
        second.world.rider.distanceMeters = 1.0;

        WorkoutGameVisualSmoother smoother;
        smoother.setTarget(first, 0);
        smoother.setTarget(second, 200);
        QCOMPARE(smoother.sample(200).simulation.activeSection, 2);
        QCOMPARE(smoother.sample(200).world.rider.distanceMeters, 1.0);
    }

    void frameRateCounterMeasuresCompletedFrameIntervals()
    {
        WorkoutGameFrameRateCounter counter;
        QCOMPARE(counter.frameRendered(0), 0.0);
        for (int timestamp = 20; timestamp <= 1000; timestamp += 20) {
            counter.frameRendered(timestamp);
        }
        QCOMPARE(counter.framesPerSecond(), 50.0);

        counter.reset();
        QCOMPARE(counter.framesPerSecond(), 0.0);
    }

    void riderContrastKeylineHasDarkAndLightEdges()
    {
        QImage sprite(1, 1, QImage::Format_ARGB32_Premultiplied);
        sprite.fill(QColor(220, 40, 30));

        const QImage outlined =
                WorkoutGameCanvas::addRiderContrastKeyline(sprite);

        QCOMPARE(outlined.size(), QSize(7, 7));
        QCOMPARE(outlined.pixelColor(3, 3), QColor(220, 40, 30));
        QCOMPARE(outlined.pixelColor(3, 2), QColor(20, 27, 31));
        QCOMPARE(outlined.pixelColor(3, 0), QColor(246, 239, 215));
        QCOMPARE(outlined.pixelColor(0, 0), QColor(246, 239, 215));
        QVERIFY(WorkoutGameCanvas::addRiderContrastKeyline(QImage()).isNull());
    }

    void fallbackSceneIsVisibleWithoutWorkout()
    {
        WorkoutGameCanvas canvas;
        canvas.resize(960, 540);

        const QImage image = render(canvas);

        QVERIFY(!image.isNull());
        QVERIFY(colors(image).size() >= 6);
        QVERIFY(qAlpha(image.pixel(10, 10)) == 255);
    }

    void activeCourseRendersRichScene()
    {
        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build(
                {{0, 10000, 250.0, 250.0}}, 200.0, 7u);
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(course, 200.0));
        WorkoutGameSimulationInput input;
        input.workoutTimeMs = 7000;
        input.actualWatts = 250.0;
        input.targetWatts = 250.0;
        input.cadenceRpm = 90.0;
        const WorkoutGameSimulationSnapshot snapshot = simulation.update(input);

        WorkoutGameCanvas canvas;
        canvas.resize(960, 540);
        canvas.setCourse(course);
        canvas.setSnapshot(snapshot, 250.0, 250.0, 90, 148, 11);
        const QImage image = render(canvas);

        QVERIFY(colors(image).size() >= 10);
        QVERIFY(image.pixelColor(20, 20) != image.pixelColor(20, 300));
        const QString outputPath = qEnvironmentVariable("GC_TEST_RENDER_OUTPUT");
        if (!outputPath.isEmpty()) QVERIFY(image.save(outputPath));
    }

    void safeBypassChangesRenderedRoute()
    {
        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build(
                {{0, 10000, 250.0, 250.0}}, 200.0, 7u);
        WorkoutGameCanvas canvas;
        canvas.resize(960, 540);
        canvas.setCourse(course);

        WorkoutGameSimulationSnapshot mainLine;
        mainLine.ready = true;
        mainLine.activeSection = 0;
        mainLine.courseProgress = 0.7;
        mainLine.route = WorkoutGameRoute::MainLine;
        canvas.setSnapshot(mainLine, 250.0, 250.0, 90, 148, 11);
        const QImage mainImage = render(canvas);

        WorkoutGameSimulationSnapshot bypass = mainLine;
        bypass.featureOutcome = WorkoutGameFeatureOutcome::Bypassed;
        bypass.route = WorkoutGameRoute::SafeBypass;
        canvas.setSnapshot(bypass, 100.0, 250.0, 50, 130, 8);
        const QImage bypassImage = render(canvas);

        QVERIFY(mainImage != bypassImage);
    }

    void physicsPoseAndDynamicCameraChangeRenderedScene()
    {
        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build(
                {{0, 30000, 140.0, 140.0}}, 200.0, 997u);
        WorkoutGameSimulationSnapshot snapshot;
        snapshot.ready = true;
        snapshot.activeSection = 0;
        snapshot.courseProgress = 0.4;
        snapshot.speedKph = 24.0;

        WorkoutGamePhysics rootsPhysics;
        QVERIFY(rootsPhysics.configure(997u));
        WorkoutGamePhysicsInput input;
        input.terrain = WorkoutGameTerrainKind::Roots;
        input.desiredSpeedMetersPerSecond = 6.0;
        input.difficulty = 0.8;
        input.effortRatio = 1.0;
        WorkoutGameWorldSnapshot world = rootsPhysics.update(input);
        for (int time = 20; time <= 3000; time += 20) {
            input.workoutTimeMs = time;
            world = rootsPhysics.update(input);
        }
        QVERIFY(world.ready);
        WorkoutGameCamera camera;
        WorkoutGameCameraSnapshot view = camera.update(world, 0.016);

        WorkoutGameCanvas canvas;
        canvas.resize(960, 540);
        canvas.setCourse(course);
        canvas.setSnapshot(snapshot, 140.0, 140.0, 82, 136, 9);
        canvas.setWorld(world, view);
        const QImage grounded = render(canvas);
        const QString groundedOutput =
                qEnvironmentVariable("GC_TEST_PHYSICS_GROUNDED_OUTPUT");
        if (!groundedOutput.isEmpty()) QVERIFY(grounded.save(groundedOutput));

        WorkoutGamePhysics dropPhysics;
        QVERIFY(dropPhysics.configure(997u));
        input = WorkoutGamePhysicsInput();
        input.terrain = WorkoutGameTerrainKind::Drop;
        input.desiredSpeedMetersPerSecond = 7.0;
        input.difficulty = 0.8;
        input.effortRatio = 1.0;
        world = dropPhysics.update(input);
        for (int time = 20; time <= 12000 && !world.rider.airborne;
             time += 20) {
            input.workoutTimeMs = time;
            world = dropPhysics.update(input);
        }
        QVERIFY(world.rider.airborne);
        view = camera.update(world, 0.1);
        canvas.setWorld(world, view);
        const QImage airborne = render(canvas);
        const QString airborneOutput =
                qEnvironmentVariable("GC_TEST_PHYSICS_AIRBORNE_OUTPUT");
        if (!airborneOutput.isEmpty()) QVERIFY(airborne.save(airborneOutput));

        QVERIFY(changedPixels(grounded, airborne) > 100);
        QVERIFY(qAlpha(airborne.pixel(0, airborne.height() - 1)) == 255);
    }

    void competitorsAndGhostChangeRenderedScene()
    {
        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build(
                {{0, 60000, 180.0, 180.0}}, 200.0, 7u);
        WorkoutGameSimulationSnapshot snapshot;
        snapshot.ready = true;
        snapshot.activeSection = 0;
        snapshot.workoutTimeMs = 30000;
        snapshot.courseProgress = 0.5;
        snapshot.speedKph = 28.0;

        WorkoutGameCanvas canvas;
        canvas.resize(960, 540);
        canvas.setCourse(course);
        canvas.setSnapshot(snapshot, 180.0, 180.0, 85, 140, 10);
        const QImage solo = render(canvas);

        WorkoutGameCompetitionSnapshot race;
        race.ready = true;
        race.playerRank = 2;
        race.totalRiders = 3;
        race.competitors = {
            {WorkoutGameCompetitorKind::Ai, 1, -1, 2900, 0.53,
             WorkoutGameRoute::MainLine},
            {WorkoutGameCompetitorKind::Ghost, 0, 1, 2500, 0.47,
             WorkoutGameRoute::SafeBypass}
        };
        canvas.setCompetition(race);
        const QImage group = render(canvas);

        QVERIFY(group != solo);
        QVERIFY(changedPixels(group, solo) > 30);
        QVERIFY(qAlpha(group.pixel(0, group.height() - 1)) == 255);
    }

    void compactSupportedSizeDoesNotClipToTransparentPixels()
    {
        WorkoutGameCanvas canvas;
        canvas.resize(canvas.minimumSize());
        const QImage image = render(canvas);

        QVERIFY(qAlpha(image.pixel(0, 0)) == 255);
        QVERIFY(qAlpha(image.pixel(image.width() - 1, image.height() - 1)) == 255);
    }

    void openGLCanvasRendersOnWindowedPlatforms()
    {
        const QString platform = QGuiApplication::platformName().toLower();
        if (platform == QStringLiteral("offscreen")
                || platform == QStringLiteral("minimal")) {
            QSKIP("QOpenGLWidget is unavailable on the active Qt platform");
        }

        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build(
                {{0, 10000, 250.0, 250.0}}, 200.0, 7u);
        WorkoutGameSimulationSnapshot snapshot;
        snapshot.ready = true;
        snapshot.activeSection = 0;
        snapshot.courseProgress = 0.7;
        snapshot.speedKph = 30.0;

        WorkoutGameOpenGLCanvas canvas;
        QSignalSpy failureSpy(&canvas, &WorkoutGameOpenGLCanvas::rendererFailed);
        canvas.resize(960, 540);
        canvas.setCourse(course);
        canvas.setSnapshot(snapshot, 250.0, 250.0, 90, 148, 11);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTest::qWait(100);

        const QImage image = canvas.grabFramebuffer();
        QCOMPARE(failureSpy.count(), 0);
        QVERIFY(!image.isNull());
        QVERIFY(colors(image).size() >= 10);
    }
};

QTEST_MAIN(TestWorkoutGameCanvas)
#include "testWorkoutGameCanvas.moc"
