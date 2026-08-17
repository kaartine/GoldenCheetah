/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameCanvas.h"
#include "Train/WorkoutGameOpenGLCanvas.h"

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

}

class TestWorkoutGameCanvas : public QObject
{
    Q_OBJECT

private slots:
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
