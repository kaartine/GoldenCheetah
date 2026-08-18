/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameOpenGLCanvas.h"

#include "WorkoutGameCanvas.h"

#include <QHideEvent>
#include <QDebug>
#include <QOpenGLContext>
#include <QPainter>
#include <QShowEvent>

#include <algorithm>

namespace {

constexpr int TargetFrameMs = 16;

}

WorkoutGameOpenGLCanvas::WorkoutGameOpenGLCanvas(QWidget *parent) :
    QOpenGLWidget(parent)
{
    setMinimumSize(320, 180);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    visualClock.start();
    animationTimer.setTimerType(Qt::PreciseTimer);
    animationTimer.setInterval(TargetFrameMs);
    connect(&animationTimer, &QTimer::timeout, this, [this]() {
        animationFrame = (animationFrame + 1) % 120;
        update();
    });
}

void WorkoutGameOpenGLCanvas::setCourse(const WorkoutGameCourse &newCourse)
{
    course = newCourse;
    current = WorkoutGameSimulationSnapshot();
    world = WorkoutGameWorldSnapshot();
    camera = WorkoutGameCameraSnapshot();
    visualSmoother.reset();
    update();
}

void WorkoutGameOpenGLCanvas::setCompetition(
        const WorkoutGameCompetitionSnapshot &newCompetition)
{
    competition = newCompetition;
    visualSmoother.setTarget(
            {current, competition, world, camera}, visualClock.elapsed());
    update();
}

void WorkoutGameOpenGLCanvas::setWorld(
        const WorkoutGameWorldSnapshot &newWorld,
        const WorkoutGameCameraSnapshot &newCamera)
{
    world = newWorld;
    camera = newCamera;
    visualSmoother.setTarget(
            {current, competition, world, camera}, visualClock.elapsed());
    update();
}

void WorkoutGameOpenGLCanvas::setFrame(
        const WorkoutGameVisualSnapshot &frame,
        double newWatts,
        double newTargetWatts,
        int newCadenceRpm,
        int newHeartRate,
        int newVirtualGear)
{
    current = frame.simulation;
    competition = frame.competition;
    world = frame.world;
    camera = frame.camera;
    watts = std::max(0.0, newWatts);
    targetWatts = std::max(0.0, newTargetWatts);
    cadenceRpm = std::max(0, newCadenceRpm);
    heartRate = std::max(0, newHeartRate);
    virtualGear = std::max(1, newVirtualGear);
    visualSmoother.setTarget(frame, visualClock.elapsed());
    if (!animationTimer.isActive()) update();
}

void WorkoutGameOpenGLCanvas::setSnapshot(
        const WorkoutGameSimulationSnapshot &snapshot,
        double newWatts,
        double newTargetWatts,
        int newCadenceRpm,
        int newHeartRate,
        int newVirtualGear)
{
    current = snapshot;
    watts = std::max(0.0, newWatts);
    targetWatts = std::max(0.0, newTargetWatts);
    cadenceRpm = std::max(0, newCadenceRpm);
    heartRate = std::max(0, newHeartRate);
    virtualGear = std::max(1, newVirtualGear);
    visualSmoother.setTarget(
            {current, competition, world, camera}, visualClock.elapsed());
    update();
}

void WorkoutGameOpenGLCanvas::initializeGL()
{
    if (!context() || !context()->isValid()) {
        reportFailure();
        return;
    }
    initializeOpenGLFunctions();
    const QSurfaceFormat format = context()->format();
    if (format.majorVersion() < 2) {
        reportFailure();
        return;
    }
    glClearColor(0.388f, 0.745f, 0.733f, 1.0f);
    const GLubyte *renderer = glGetString(GL_RENDERER);
    const QString rendererDescription = renderer
            ? QString::fromLatin1(reinterpret_cast<const char *>(renderer))
            : QStringLiteral("unknown");
    const QString normalized = rendererDescription.toLower();
    rendererLabel = normalized.contains(QStringLiteral("llvmpipe"))
            || normalized.contains(QStringLiteral("software"))
            ? QStringLiteral("GL SW")
            : QStringLiteral("GL");
    qInfo().noquote() << "Workout Game OpenGL renderer:"
                      << rendererDescription;
}

void WorkoutGameOpenGLCanvas::paintGL()
{
    if (!context() || !context()->isValid()) {
        reportFailure();
        return;
    }
    glClear(GL_COLOR_BUFFER_BIT);
    const std::int64_t nowMs = visualClock.elapsed();
    const WorkoutGameVisualSnapshot visual = visualSmoother.sample(nowMs);
    const double fps = frameRateCounter.frameRendered(nowMs);
    QPainter painter(this);
    WorkoutGameCanvas::paintScene(
            painter, rect(), course, visual.simulation, visual.competition,
            visual.world, visual.camera,
            watts, targetWatts, cadenceRpm, heartRate, virtualGear,
            animationFrame, fps, rendererLabel);
}

void WorkoutGameOpenGLCanvas::showEvent(QShowEvent *event)
{
    QOpenGLWidget::showEvent(event);
    animationTimer.start();
}

void WorkoutGameOpenGLCanvas::hideEvent(QHideEvent *event)
{
    animationTimer.stop();
    QOpenGLWidget::hideEvent(event);
}

void WorkoutGameOpenGLCanvas::reportFailure()
{
    if (failureReported) return;
    failureReported = true;
    animationTimer.stop();
    QTimer::singleShot(0, this, [this]() { emit rendererFailed(); });
}
