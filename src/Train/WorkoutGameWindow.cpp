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
#include "Settings.h"
#include "Zones.h"

#include <QDate>
#include <QGuiApplication>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int MaximumStoredGhosts = 12;

QString ghostKey(std::uint32_t seed)
{
    return QStringLiteral(
            GC_QSETTINGS_ATHLETE_PRIVATE "workoutGame/ghost/%1")
            .arg(seed, 8, 16, QLatin1Char('0'));
}

QString ghostIndexKey()
{
    return QStringLiteral(
            GC_QSETTINGS_ATHLETE_PRIVATE "workoutGame/ghostIndex");
}

QString ghostSeedText(std::uint32_t seed)
{
    return QStringLiteral("%1").arg(seed, 8, 16, QLatin1Char('0'));
}

}

WorkoutGameWindow::WorkoutGameWindow(Context *context) :
    GcChartWindow(context),
    context(context),
    renderStack(new QStackedWidget(this)),
    painterCanvas(new WorkoutGameCanvas(renderStack)),
    openGLCanvas(new WorkoutGameOpenGLCanvas(renderStack))
{
    setContentsMargins(0, 0, 0, 0);
    setAccessibleName(tr("Workout game view"));
    setProperty("color", QColor(20, 27, 31));
    setProperty("title", tr("Workout Game"));

    QVBoxLayout *layout = new QVBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setChartLayout(layout);
    painterCanvas->setAccessibleName(tr("Workout game canvas"));
    openGLCanvas->setAccessibleName(tr("Workout game canvas"));
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
    connect(context, &Context::stop, this, &WorkoutGameWindow::stop);

    ergFileSelected(context->currentErgFile());
}

WorkoutGameGhostReplay WorkoutGameWindow::loadGhost(
        const WorkoutGameCourse &course) const
{
    WorkoutGameGhostReplay replay;
    if (!context || !context->athlete || !appsettings
            || course.status != WorkoutGameCourseStatus::Ready) {
        return replay;
    }

    const QString stored = appsettings->cvalue(
            context->athlete->cyclist,
            ghostKey(course.seed),
            QString()).toString();
    WorkoutGameGhostCodec::decode(stored.toStdString(), replay);
    return replay;
}

void WorkoutGameWindow::storeGhost()
{
    if (!context || !context->athlete || !appsettings
            || currentCourse.status != WorkoutGameCourseStatus::Ready) {
        return;
    }

    const WorkoutGameGhostReplay candidate = ghostRecorder.replay();
    const WorkoutGameGhostReplay stored = loadGhost(currentCourse);
    if (!WorkoutGameGhostCodec::isBetter(candidate, stored)) return;

    const std::string encoded = WorkoutGameGhostCodec::encode(candidate);
    if (encoded.empty()) return;
    const QString cyclist = context->athlete->cyclist;
    const QString dataKey = ghostKey(currentCourse.seed);
    if (!appsettings->setCValueChecked(
            cyclist, dataKey, QString::fromStdString(encoded))) {
        return;
    }

    QStringList index = appsettings->cvalue(
            cyclist, ghostIndexKey(), QStringList()).toStringList();
    const QString seed = ghostSeedText(currentCourse.seed);
    index.removeAll(seed);
    index.prepend(seed);
    while (index.size() > MaximumStoredGhosts) {
        const QString retired = index.takeLast();
        bool ok = false;
        const std::uint32_t retiredSeed = retired.toUInt(&ok, 16);
        if (ok) {
            appsettings->setCValueChecked(
                    cyclist, ghostKey(retiredSeed), QString());
        }
    }
    if (!appsettings->setCValueChecked(cyclist, ghostIndexKey(), index)
            || !appsettings->syncCValueChecked(cyclist, dataKey)) {
        return;
    }
    competition.configure(currentCourse, candidate);
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
    currentCourse = WorkoutGameCourse();
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
            currentCourse = WorkoutGameCourseBuilder::build(
                    normalized.intervals, ftpWatts);
        }
    }

    simulation.configure(currentCourse, ftpWatts);
    physics.configure(currentCourse.seed);
    camera.reset();
    competition.configure(currentCourse, loadGhost(currentCourse));
    ghostRecorder.configure(currentCourse.seed, currentCourse.durationMs);
    painterCanvas->setCourse(currentCourse);
    openGLCanvas->setCourse(currentCourse);
    hasTelemetry = false;
    paused = false;
    sessionActive = false;
    worldClockInitialized = false;
    lastWorldTimeMs = 0;
    updateSimulation(0);
}

void WorkoutGameWindow::telemetryUpdate(const RealtimeData &telemetry)
{
    latestTelemetry = telemetry;
    hasTelemetry = true;
    const int cadenceRpm = int(std::lround(latestTelemetry.getCadence()));
    const int heartRate = int(std::lround(latestTelemetry.getHr()));
    const int virtualGear = std::max(1, latestTelemetry.getVirtualGear());
    painterCanvas->setTelemetry(
            latestTelemetry.getWatts(), latestTelemetry.getLoad(),
            cadenceRpm, heartRate, virtualGear);
    openGLCanvas->setTelemetry(
            latestTelemetry.getWatts(), latestTelemetry.getLoad(),
            cadenceRpm, heartRate, virtualGear);
}

void WorkoutGameWindow::setNow(long workoutTimeMs)
{
    updateSimulation(workoutTimeMs);
}

void WorkoutGameWindow::start()
{
    simulation.reset();
    physics.reset();
    camera.reset();
    ghostRecorder.configure(currentCourse.seed, currentCourse.durationMs);
    paused = false;
    sessionActive = true;
    worldClockInitialized = false;
    lastWorldTimeMs = 0;
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

void WorkoutGameWindow::stop()
{
    sessionActive = false;
    storeGhost();
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
    WorkoutGameWorldSnapshot world;
    WorkoutGameCameraSnapshot view;
    if (snapshot.ready
            && snapshot.activeSection >= 0
            && snapshot.activeSection < int(currentCourse.sections.size())) {
        const WorkoutGameSection &section =
                currentCourse.sections[snapshot.activeSection];
        const double target = std::max(
                1.0,
                input.targetWatts > 0.0
                        ? input.targetWatts
                        : section.targetWatts);
        WorkoutGamePhysicsInput physicsInput;
        physicsInput.workoutTimeMs = snapshot.workoutTimeMs;
        physicsInput.terrain = section.terrain;
        physicsInput.desiredSpeedMetersPerSecond = snapshot.speedKph / 3.6;
        physicsInput.gradePercent = section.gradePercent;
        physicsInput.difficulty = section.difficulty;
        physicsInput.effortRatio = std::max(0.0, input.actualWatts) / target;
        physicsInput.paused = paused;
        physicsInput.jumpRequested = section.terrain
                    == WorkoutGameTerrainKind::BunnyHop
                && physicsInput.effortRatio >= 0.9
                && input.cadenceRpm >= 65.0;
        world = physics.update(physicsInput);

        const double cameraElapsedSeconds = worldClockInitialized
                && snapshot.workoutTimeMs >= lastWorldTimeMs && !paused
                ? double(snapshot.workoutTimeMs - lastWorldTimeMs) / 1000.0
                : 0.0;
        view = camera.update(world, cameraElapsedSeconds);
        worldClockInitialized = true;
        lastWorldTimeMs = snapshot.workoutTimeMs;
    } else {
        camera.reset();
        worldClockInitialized = false;
        lastWorldTimeMs = workoutTimeMs;
    }
    const WorkoutGameCompetitionSnapshot race = competition.update(snapshot);
    if (sessionActive) ghostRecorder.record(snapshot);
    const WorkoutGameVisualSnapshot frame = {snapshot, race, world, view};
    painterCanvas->setFrame(
            frame,
            input.actualWatts,
            input.targetWatts,
            int(std::lround(input.cadenceRpm)),
            hasTelemetry ? int(std::lround(latestTelemetry.getHr())) : 0,
            std::max(1, input.virtualGear));
    openGLCanvas->setFrame(
            frame,
            input.actualWatts,
            input.targetWatts,
            int(std::lround(input.cadenceRpm)),
            hasTelemetry ? int(std::lround(latestTelemetry.getHr())) : 0,
            std::max(1, input.virtualGear));
}
