/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameWindow.h"

#include "Athlete.h"
#include "Context.h"
#include "ErgFile.h"
#include "ChartSpace.h"
#include "WorkoutGameCanvas.h"
#include "WorkoutGameCourse.h"
#include "WorkoutGameOpenGLCanvas.h"
#include "WorkoutGameRendererPolicy.h"
#include "WorkoutGameWorkoutAdapter.h"
#include "Zones.h"

#include <QDate>
#include <QGuiApplication>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <vector>

WorkoutGameWindow::WorkoutGameWindow(Context *context) :
    GcChartWindow(context),
    context(context),
    renderStack(new QStackedWidget(this)),
    painterCanvas(new WorkoutGameCanvas(renderStack)),
    openGLCanvas(new WorkoutGameOpenGLCanvas(renderStack))
{
    setContentsMargins(0, 0, 0, 0);
    setProperty("color", QColor(20, 27, 31));
    setProperty("title", tr("Workout Game"));

    QVBoxLayout *layout = new QVBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setChartLayout(layout);
    renderStack->addWidget(painterCanvas);
    renderStack->addWidget(openGLCanvas);
    layout->addWidget(renderStack);

    const bool forcePainter = qEnvironmentVariableIntValue(
            "GC_WORKOUT_GAME_FORCE_PAINTER") != 0;
    const WorkoutGameRendererBackend backend = WorkoutGameRendererPolicy::choose(
            forcePainter,
            QGuiApplication::platformName().toStdString(),
            gl_major);
    renderStack->setCurrentWidget(
            backend == WorkoutGameRendererBackend::OpenGL
                    ? static_cast<QWidget *>(openGLCanvas)
                    : static_cast<QWidget *>(painterCanvas));
    connect(openGLCanvas, &WorkoutGameOpenGLCanvas::rendererFailed,
            this, &WorkoutGameWindow::usePainterFallback);

    connect(context, qOverload<ErgFile *>(&Context::ergFileSelected),
            this, &WorkoutGameWindow::ergFileSelected);
    connect(context, &Context::telemetryUpdate,
            this, &WorkoutGameWindow::telemetryUpdate);
    connect(context, &Context::setNow, this, &WorkoutGameWindow::setNow);
    connect(context, &Context::start, this, &WorkoutGameWindow::start);
    connect(context, &Context::pause, this, &WorkoutGameWindow::pause);
    connect(context, &Context::unpause, this, &WorkoutGameWindow::unpause);

    ergFileSelected(context->currentErgFile());
}

double WorkoutGameWindow::currentFtp(ErgFile *workout) const
{
    if (context && context->athlete && context->athlete->zones("Bike")) {
        const Zones *zones = context->athlete->zones("Bike");
        const int range = zones->whichRange(QDate::currentDate());
        if (range >= 0 && zones->getCP(range) > 0) return zones->getCP(range);
    }
    if (workout && workout->ftp() > 0) return workout->ftp();
    if (workout && workout->CP() > 0.0) return workout->CP();
    return 0.0;
}

void WorkoutGameWindow::ergFileSelected(ErgFile *workout)
{
    WorkoutGameCourse course;
    ftpWatts = currentFtp(workout);
    if (workout && workout->hasWatts() && ftpWatts > 0.0) {
        std::vector<WorkoutGamePowerPoint> points;
        points.reserve(workout->Points.size());
        for (const ErgFilePoint &point : workout->Points) {
            points.push_back({point.x, point.val});
        }
        const WorkoutGameWorkout normalized =
                WorkoutGameWorkoutAdapter::normalize(points);
        if (normalized.status == WorkoutGameWorkoutStatus::Ready) {
            course = WorkoutGameCourseBuilder::build(
                    normalized.intervals, ftpWatts);
        }
    }

    simulation.configure(course, ftpWatts);
    painterCanvas->setCourse(course);
    openGLCanvas->setCourse(course);
    hasTelemetry = false;
    paused = false;
    updateSimulation(0);
}

void WorkoutGameWindow::telemetryUpdate(const RealtimeData &telemetry)
{
    latestTelemetry = telemetry;
    hasTelemetry = true;
    updateSimulation(context->getNow());
}

void WorkoutGameWindow::setNow(long workoutTimeMs)
{
    updateSimulation(workoutTimeMs);
}

void WorkoutGameWindow::start()
{
    simulation.reset();
    paused = false;
    updateSimulation(context->getNow());
}

void WorkoutGameWindow::pause()
{
    paused = true;
    updateSimulation(context->getNow());
}

void WorkoutGameWindow::unpause()
{
    paused = false;
    updateSimulation(context->getNow());
}

void WorkoutGameWindow::usePainterFallback()
{
    renderStack->setCurrentWidget(painterCanvas);
}

void WorkoutGameWindow::updateSimulation(std::int64_t workoutTimeMs)
{
    WorkoutGameSimulationInput input;
    input.workoutTimeMs = workoutTimeMs;
    input.paused = paused;
    if (hasTelemetry) {
        input.actualWatts = latestTelemetry.getWatts();
        input.targetWatts = latestTelemetry.getLoad();
        input.cadenceRpm = latestTelemetry.getCadence();
        input.virtualGear = latestTelemetry.getVirtualGear();
    }

    const WorkoutGameSimulationSnapshot snapshot = simulation.update(input);
    painterCanvas->setSnapshot(
            snapshot,
            input.actualWatts,
            input.targetWatts,
            int(std::lround(input.cadenceRpm)),
            hasTelemetry ? int(std::lround(latestTelemetry.getHr())) : 0,
            std::max(1, input.virtualGear));
    openGLCanvas->setSnapshot(
            snapshot,
            input.actualWatts,
            input.targetWatts,
            int(std::lround(input.cadenceRpm)),
            hasTelemetry ? int(std::lround(latestTelemetry.getHr())) : 0,
            std::max(1, input.virtualGear));
}
