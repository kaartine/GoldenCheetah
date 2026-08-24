/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DWindow.h"

#include <QQmlContext>
#include <QQmlError>
#include <QDebug>
#include <QStringList>
#include <QUrl>

WorkoutGame3DWindow::WorkoutGame3DWindow(
        bool rendererEnabled,
        QWindow *parent) :
    QQuickView(parent),
    viewModel(new WorkoutGame3DViewModel(this))
{
    setResizeMode(QQuickView::SizeRootObjectToView);
    setColor(QColor(105, 154, 184));
    rootContext()->setContextProperty(
            QStringLiteral("workoutGame3D"), viewModel);
    connect(this, &QQuickView::statusChanged,
            this, &WorkoutGame3DWindow::handleStatusChanged);
    connect(this, &QQuickWindow::frameSwapped, this, [this]() {
        if (!sessionRunning) return;
        const double fps = frameRateCounter.frameRenderedNanoseconds(
                monotonicClock.nsecsElapsed());
        viewModel->setFps(fps);
        presentFrame();
    }, Qt::QueuedConnection);
    connect(this, &QQuickWindow::sceneGraphError,
            this, &WorkoutGame3DWindow::handleSceneGraphError);
    monotonicClock.start();
    if (rendererEnabled) {
        setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
    }
}

WorkoutGame3DWindow::~WorkoutGame3DWindow()
{
    disconnect(this, nullptr, nullptr, nullptr);
    setSource(QUrl());
    releaseResources();
}

void WorkoutGame3DWindow::setCourse(
        const WorkoutGameCourse &course,
        double ftpWatts)
{
    visualSmoother.reset();
    hasFrame = false;
    viewModel->setCourse(course, ftpWatts);
}

void WorkoutGame3DWindow::setFrame(
        const WorkoutGameVisualSnapshot &frame,
        double newWatts,
        double newTargetWatts,
        int newCadenceRpm,
        int newHeartRate,
        int newVirtualGear)
{
    watts = newWatts;
    targetWatts = newTargetWatts;
    cadenceRpm = newCadenceRpm;
    heartRate = newHeartRate;
    virtualGear = newVirtualGear;
    const bool firstFrame = !hasFrame;
    visualSmoother.setTarget(
            frame, monotonicClock.elapsed());
    hasFrame = true;
    if (!sessionRunning || firstFrame) presentFrame();
    if (sessionRunning) requestUpdate();
}

void WorkoutGame3DWindow::setTelemetry(
        double newWatts,
        double newTargetWatts,
        int newCadenceRpm,
        int newHeartRate,
        int newVirtualGear)
{
    watts = newWatts;
    targetWatts = newTargetWatts;
    cadenceRpm = newCadenceRpm;
    heartRate = newHeartRate;
    virtualGear = newVirtualGear;
    viewModel->setTelemetry(
            watts, targetWatts, cadenceRpm, heartRate, virtualGear);
}

void WorkoutGame3DWindow::setSessionRunning(bool running)
{
    sessionRunning = running;
    if (running) {
        frameRateCounter.reset();
        presentFrame();
    }
}

void WorkoutGame3DWindow::presentFrame()
{
    if (!hasFrame) return;
    viewModel->setFrame(
            visualSmoother.sample(monotonicClock.elapsed()),
            watts, targetWatts, cadenceRpm, heartRate, virtualGear);
}

void WorkoutGame3DWindow::handleStatusChanged(QQuickView::Status status)
{
    if (status != QQuickView::Error || failureReported) return;
    const QList<QQmlError> loadErrors = errors();
    QStringList messages;
    for (const QQmlError &error : loadErrors) {
        messages.push_back(error.toString());
    }
    reportFailure(messages.join(QLatin1Char('\n')));
}

void WorkoutGame3DWindow::handleSceneGraphError(
        QQuickWindow::SceneGraphError,
        const QString &message)
{
    reportFailure(message);
}

void WorkoutGame3DWindow::reportFailure(const QString &message)
{
    if (failureReported) return;
    failureReported = true;
    qWarning().noquote() << "Workout Game 3D renderer error:" << message;
    emit rendererFailed();
}
