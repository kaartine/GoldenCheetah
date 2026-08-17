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
    animationTimer.setInterval(TargetFrameMs);
    connect(&animationTimer, &QTimer::timeout, this, [this]() {
        animationFrame = (animationFrame + 1) % 120;
        update();
    });
}

void WorkoutGameOpenGLCanvas::setCourse(const WorkoutGameCourse &newCourse)
{
    course = newCourse;
    update();
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
}

void WorkoutGameOpenGLCanvas::paintGL()
{
    if (!context() || !context()->isValid()) {
        reportFailure();
        return;
    }
    glClear(GL_COLOR_BUFFER_BIT);
    QPainter painter(this);
    WorkoutGameCanvas::paintScene(
            painter, rect(), course, current, watts, targetWatts,
            cadenceRpm, heartRate, virtualGear, animationFrame);
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
