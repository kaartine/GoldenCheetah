/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameWindow.h"
#include "WorkoutGameFeatureLab.h"

#include "Athlete.h"
#include "Context.h"
#include "ErgFile.h"
#include "ChartSpace.h"
#include "WorkoutGame3DWindow.h"
#include "WorkoutGameCanvas.h"
#include "WorkoutGameCourse.h"
#include "WorkoutGameOpenGLCanvas.h"
#include "WorkoutGameRendererPolicy.h"
#include "WorkoutGameSceneGraphWindow.h"
#include "WorkoutGameWorkoutAdapter.h"
#include "Settings.h"
#include "VirtualDrivetrain.h"
#include "Zones.h"

#include <QDate>
#include <QGuiApplication>
#include <QHideEvent>
#include <QDebug>
#include <QStackedWidget>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int MaximumStoredGhosts = 12;
constexpr double GameWheelCircumferenceMeters = 2.12;
constexpr double MaximumPowerWatts = 10000.0;
constexpr double MaximumCadenceRpm = 300.0;
constexpr double MaximumSpeedKph = 300.0;

double finiteClampedNonNegative(double value, double maximum)
{
    return std::isfinite(value)
            ? std::clamp(value, 0.0, maximum) : 0.0;
}

int heartRateValue(double value)
{
    return std::isfinite(value)
            ? int(std::lround(std::clamp(value, 0.0, 300.0))) : 0;
}

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

bool threeDRendererRequested()
{
    return qEnvironmentVariableIntValue("GC_WORKOUT_GAME_3D") != 0
            && qEnvironmentVariableIntValue(
                    "GC_WORKOUT_GAME_FORCE_PAINTER") == 0;
}

}

WorkoutGameWindow::WorkoutGameWindow(Context *context) :
    GcChartWindow(context),
    context(context),
    renderStack(new QStackedWidget(this)),
    threeDWindow(new WorkoutGame3DWindow(threeDRendererRequested())),
    threeDContainer(QWidget::createWindowContainer(
            threeDWindow, renderStack)),
    sceneGraphWindow(new WorkoutGameSceneGraphWindow),
    sceneGraphContainer(QWidget::createWindowContainer(
            sceneGraphWindow, renderStack)),
    painterCanvas(new WorkoutGameCanvas(renderStack)),
    openGLCanvas(new WorkoutGameOpenGLCanvas(renderStack)),
    frameDrainTimer(new QTimer(this))
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
    threeDContainer->setAccessibleName(tr("Workout game 3D canvas"));
    threeDContainer->setMinimumSize(320, 180);
    sceneGraphContainer->setAccessibleName(tr("Workout game canvas"));
    sceneGraphContainer->setMinimumSize(320, 180);
    renderStack->addWidget(painterCanvas);
    renderStack->addWidget(openGLCanvas);
    renderStack->addWidget(sceneGraphContainer);
    renderStack->addWidget(threeDContainer);
    layout->addWidget(renderStack);

    const bool forcePainter = qEnvironmentVariableIntValue(
            "GC_WORKOUT_GAME_FORCE_PAINTER") != 0;
    const WorkoutGameRendererDecision rendererDecision =
            WorkoutGameRendererPolicy::decide(
            forcePainter,
            QGuiApplication::platformName().toStdString(),
            gl_major);
    const WorkoutGameRendererBackend backend = rendererDecision.backend;
    qInfo().noquote()
            << "Workout Game renderer selection:"
            << WorkoutGameRendererPolicy::backendName(backend)
            << "reason="
            << WorkoutGameRendererPolicy::reasonName(rendererDecision.reason)
            << "platform=" << QGuiApplication::platformName()
            << "OpenGL=" << gl_major;
    const bool useThreeD = threeDRendererRequested();
    if (useThreeD) {
        renderStack->setCurrentWidget(threeDContainer);
        qInfo().noquote() << "Workout Game renderer selection: Qt Quick 3D";
    } else switch (backend) {
    case WorkoutGameRendererBackend::SceneGraph:
        renderStack->setCurrentWidget(sceneGraphContainer);
        break;
    case WorkoutGameRendererBackend::OpenGL:
        renderStack->setCurrentWidget(openGLCanvas);
        break;
    case WorkoutGameRendererBackend::Painter:
        renderStack->setCurrentWidget(painterCanvas);
        break;
    }
    connect(sceneGraphWindow, &WorkoutGameSceneGraphWindow::rendererFailed,
            this, &WorkoutGameWindow::useOpenGLFallback);
    connect(threeDWindow, &WorkoutGame3DWindow::rendererFailed,
            this, &WorkoutGameWindow::useSceneGraphFallback);
    connect(openGLCanvas, &WorkoutGameOpenGLCanvas::rendererFailed,
            this, &WorkoutGameWindow::usePainterFallback);
    if (useThreeD && !threeDWindow->rendererAvailable()) {
        useSceneGraphFallback();
    }

    connect(context, qOverload<ErgFile *>(&Context::ergFileSelected),
            this, &WorkoutGameWindow::ergFileSelected);
    connect(context, &Context::telemetryUpdate,
            this, &WorkoutGameWindow::telemetryUpdate);
    connect(context, &Context::setNow, this, &WorkoutGameWindow::setNow);
    connect(context, &Context::start, this, &WorkoutGameWindow::start);
    connect(context, &Context::pause, this, &WorkoutGameWindow::pause);
    connect(context, &Context::unpause, this, &WorkoutGameWindow::unpause);
    connect(context, &Context::stop, this, &WorkoutGameWindow::stop);
    connect(context, &Context::configChanged,
            this, &WorkoutGameWindow::configChanged);

    configChanged(0);

    frameDrainTimer->setTimerType(Qt::PreciseTimer);
    frameDrainTimer->setInterval(16);
    connect(frameDrainTimer, &QTimer::timeout,
            this, &WorkoutGameWindow::drainRunnerFrame);

    ergFileSelected(context->currentErgFile());
}

void WorkoutGameWindow::showEvent(QShowEvent *event)
{
    GcChartWindow::showEvent(event);
    if (sessionActive && !paused && presentationSuspended) {
        updateAtWorkoutPosition(context->getNow());
        runner.resume(currentWorkoutTimeMs, currentAnchorRate);
        presentationSuspended = false;
    }
    frameDrainTimer->start();
    drainRunnerFrame();
    sceneGraphWindow->setSessionRunning(
            sessionActive && !paused
            && renderStack->currentWidget() == sceneGraphContainer);
    threeDWindow->setSessionRunning(
            sessionActive && !paused
            && renderStack->currentWidget() == threeDContainer);
}

void WorkoutGameWindow::hideEvent(QHideEvent *event)
{
    frameDrainTimer->stop();
    threeDWindow->setSessionRunning(false);
    sceneGraphWindow->setSessionRunning(false);
    if (sessionActive && !paused && !presentationSuspended) {
        drainRunnerFrame();
        updateAtWorkoutPosition(context->getNow());
        runner.pause(currentWorkoutTimeMs);
        presentationSuspended = true;
    }
    GcChartWindow::hideEvent(event);
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
    audioFeedback.reset();
    sessionState.workoutSelected();
    currentCourse = WorkoutGameCourse();
    distanceRuntime.reset();
    distanceSnapshot = WorkoutGameDistancePlaybackSnapshot();
    ftpWatts = currentFtp(workout);
    featureLabEnabled = qEnvironmentVariableIntValue(
            "GC_WORKOUT_GAME_FEATURE_LAB") != 0;
    if (featureLabEnabled) {
        if (ftpWatts <= 0.0) ftpWatts = 200.0;
        currentCourse = WorkoutGameFeatureLab::course(ftpWatts);
    } else if (workout
            && workout->format() == ErgFileFormat::crs
            && distanceRuntime.configure(workout->filename())
                == WorkoutGameCourseRuntimeStatus::Ready) {
        currentCourse = distanceRuntime.visualCourse();
        ftpWatts = distanceRuntime.ftpWatts();
    } else if (workout && workout->hasWatts() && ftpWatts > 0.0) {
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

    runner.configure(currentCourse, ftpWatts, featureLabEnabled);
    competition.configure(currentCourse, loadGhost(currentCourse));
    ghostRecorder.configure(currentCourse.seed, currentCourse.durationMs);
    painterCanvas->setCourse(currentCourse);
    openGLCanvas->setCourse(currentCourse);
    sceneGraphWindow->setCourse(currentCourse, ftpWatts);
    if (renderStack->currentWidget() == threeDContainer) {
        threeDWindow->setCourse(currentCourse, ftpWatts);
    }
    hasTelemetry = false;
    hasFrame = false;
    lastFrame = WorkoutGameEngineFrame();
    paused = false;
    sessionActive = false;
    presentationSuspended = false;
    positionRate.reset(1.0);
    currentWorkoutTimeMs = 0;
    lastTelemetryMonotonicTimeMs = -1;
    currentAnchorRate = 1.0;
    updateAtWorkoutPosition(0);
    drainRunnerFrame();
}

void WorkoutGameWindow::telemetryUpdate(const RealtimeData &telemetry)
{
    latestTelemetry = telemetry;
    hasTelemetry = true;
    const QString sourceName = QString::fromLatin1(telemetry.getName());
    const QString generatorPrefix = QStringLiteral("Data Generator [");
    const QString generatorState = sourceName.startsWith(generatorPrefix)
            && sourceName.endsWith(QLatin1Char(']'))
            ? sourceName.mid(generatorPrefix.size(),
                             sourceName.size() - generatorPrefix.size() - 1)
            : QString();
    threeDWindow->setGeneratorState(generatorState);
    lastTelemetryMonotonicTimeMs =
            WorkoutGameRunner::monotonicMilliseconds();
    updateRunnerTelemetry();
}

void WorkoutGameWindow::setNow(long workoutPosition)
{
    if (!sessionState.acceptsPositionUpdate(context->isRunning())) return;
    updateAtWorkoutPosition(workoutPosition);
}

void WorkoutGameWindow::start()
{
    audioFeedback.reset();
    sessionState.started();
    ghostRecorder.configure(currentCourse.seed, currentCourse.durationMs);
    hasFrame = false;
    lastFrame = WorkoutGameEngineFrame();
    paused = false;
    sessionActive = true;
    sceneGraphWindow->setSessionRunning(
            isVisible() && renderStack->currentWidget() == sceneGraphContainer);
    threeDWindow->setSessionRunning(
            isVisible() && renderStack->currentWidget() == threeDContainer);
    updateAtWorkoutPosition(context->getNow());
    runner.start(currentWorkoutTimeMs, currentAnchorRate);
    if (!isVisible()) {
        runner.pause(currentWorkoutTimeMs);
        presentationSuspended = true;
    } else {
        presentationSuspended = false;
    }
}

void WorkoutGameWindow::pause()
{
    drainRunnerFrame();
    paused = true;
    threeDWindow->setSessionRunning(false);
    sceneGraphWindow->setSessionRunning(false);
    updateAtWorkoutPosition(context->getNow());
    runner.pause(currentWorkoutTimeMs);
}

void WorkoutGameWindow::unpause()
{
    paused = false;
    sceneGraphWindow->setSessionRunning(
            sessionActive && isVisible()
            && renderStack->currentWidget() == sceneGraphContainer);
    threeDWindow->setSessionRunning(
            sessionActive && isVisible()
            && renderStack->currentWidget() == threeDContainer);
    updateAtWorkoutPosition(context->getNow());
    if (isVisible()) {
        runner.resume(currentWorkoutTimeMs, currentAnchorRate);
        presentationSuspended = false;
    } else {
        presentationSuspended = true;
    }
}

void WorkoutGameWindow::stop()
{
    sessionState.stopped();
    threeDWindow->setSessionRunning(false);
    sceneGraphWindow->setSessionRunning(false);
    sessionActive = false;
    audioFeedback.reset();
    WorkoutGameEngineFrame finalFrame;
    if (runner.stopAndTakeLatest(currentWorkoutTimeMs, finalFrame)) {
        ghostRecorder.record(finalFrame.visual.simulation);
        lastFrame = finalFrame;
        hasFrame = true;
        displayFrame(finalFrame);
    }
    presentationSuspended = false;
    storeGhost();
}

void WorkoutGameWindow::configChanged(qint32)
{
    audioFeedback.setEnabled(appsettings->value(
            this, TRAIN_WORKOUT_GAME_AUDIO, false).toBool());
}

void WorkoutGameWindow::useSceneGraphFallback()
{
    if (renderStack->currentWidget() != threeDContainer) return;
    qWarning().noquote()
            << "Workout Game renderer fallback: Qt Quick 3D -> SceneGraph";
    threeDWindow->setSessionRunning(false);
    renderStack->setCurrentWidget(sceneGraphContainer);
    sceneGraphWindow->setSessionRunning(sessionActive && !paused && isVisible());
    if (hasFrame) displayFrame(lastFrame);
}

void WorkoutGameWindow::usePainterFallback()
{
    qWarning().noquote()
            << "Workout Game renderer fallback: OpenGL -> Painter";
    sceneGraphWindow->setSessionRunning(false);
    renderStack->setCurrentWidget(painterCanvas);
    if (hasFrame) displayFrame(lastFrame);
}

void WorkoutGameWindow::useOpenGLFallback()
{
    qWarning().noquote()
            << "Workout Game renderer fallback: SceneGraph -> OpenGL";
    sceneGraphWindow->setSessionRunning(false);
    renderStack->setCurrentWidget(openGLCanvas);
    if (hasFrame) displayFrame(lastFrame);
}

void WorkoutGameWindow::updateAtWorkoutPosition(
        std::int64_t workoutPosition)
{
    if (distanceRuntime.enabled()) {
        distanceSnapshot = distanceRuntime.atWorkoutPosition(workoutPosition);
        currentWorkoutTimeMs = distanceSnapshot.ready
                ? distanceSnapshot.nominalTimeMs : 0;
    } else {
        currentWorkoutTimeMs = std::max<std::int64_t>(0, workoutPosition);
    }
    const std::int64_t nowMs = WorkoutGameRunner::monotonicMilliseconds();
    currentAnchorRate = anchorRate(currentWorkoutTimeMs, nowMs);
    runner.setAnchor(currentWorkoutTimeMs, currentAnchorRate);
    if (featureLabEnabled) {
        context->notifyWorkoutGameGeneratorTarget(
                WorkoutGameFeatureLab::targetWattsAt(
                    currentCourse, currentWorkoutTimeMs));
    }
    updateRunnerTelemetry();
}

double WorkoutGameWindow::anchorRate(
        std::int64_t workoutTimeMs,
        std::int64_t monotonicTimeMs)
{
    if (!distanceRuntime.enabled()) return 1.0;
    const bool moving = hasTelemetry
            && (finiteClampedNonNegative(
                    latestTelemetry.getSpeed(), MaximumSpeedKph) > 0.2
                || finiteClampedNonNegative(
                    latestTelemetry.getCadence(), MaximumCadenceRpm) > 0.0
                || finiteClampedNonNegative(
                    latestTelemetry.getWatts(), MaximumPowerWatts) > 5.0);
    return positionRate.update({
        workoutTimeMs,
        monotonicTimeMs,
        hasTelemetry,
        moving
    });
}

void WorkoutGameWindow::updateRunnerTelemetry()
{
    WorkoutGameEngineInput input;
    input.simulation.workoutTimeMs = currentWorkoutTimeMs;
    input.simulation.paused = paused;
    input.telemetryMonotonicTimeMs = lastTelemetryMonotonicTimeMs;
    if (hasTelemetry) {
        input.simulation.actualWatts = finiteClampedNonNegative(
                latestTelemetry.getWatts(), MaximumPowerWatts);
        input.simulation.targetWatts = distanceRuntime.enabled()
                && distanceSnapshot.ready
                ? distanceSnapshot.targetWatts
                : finiteClampedNonNegative(
                    latestTelemetry.getLoad(), MaximumPowerWatts);
        input.simulation.cadenceRpm = finiteClampedNonNegative(
                latestTelemetry.getCadence(), MaximumCadenceRpm);
        if (distanceRuntime.enabled()) {
            input.simulation.authoritativeSpeedKph = finiteClampedNonNegative(
                    latestTelemetry.getSpeed(), MaximumSpeedKph);
            const VirtualDrivetrain drivetrain(
                    std::max(1, latestTelemetry.getVirtualGear()));
            if (input.simulation.cadenceRpm > 0.0) {
                input.simulation.drivetrainSpeedLimitKph = drivetrain.speedKph(
                        input.simulation.cadenceRpm,
                        GameWheelCircumferenceMeters);
            }
        }
        input.simulation.virtualGear = latestTelemetry.getVirtualGear();
        input.heartRate = heartRateValue(latestTelemetry.getHr());
    }
    runner.setTelemetry(input);
}

void WorkoutGameWindow::drainRunnerFrame()
{
    WorkoutGameEngineFrame frame;
    if (!runner.takeLatest(frame)) return;
    if (sessionActive) ghostRecorder.record(frame.visual.simulation);
    lastFrame = frame;
    hasFrame = true;
    displayFrame(frame);
}

void WorkoutGameWindow::displayFrame(const WorkoutGameEngineFrame &frame)
{
    if (sessionActive && !paused && isVisible()) {
        audioFeedback.update(frame.audioEvents);
    }
    QWidget *renderer = renderStack->currentWidget();
    if (renderer == painterCanvas) {
        painterCanvas->setFrame(
                frame.visual, frame.watts, frame.targetWatts,
                frame.cadenceRpm, frame.heartRate, frame.virtualGear);
    } else if (renderer == openGLCanvas) {
        openGLCanvas->setFrame(
                frame.visual, frame.watts, frame.targetWatts,
                frame.cadenceRpm, frame.heartRate, frame.virtualGear);
    } else if (renderer == sceneGraphContainer) {
        sceneGraphWindow->setFrame(
                frame.visual, frame.watts, frame.targetWatts,
                frame.cadenceRpm, frame.heartRate, frame.virtualGear);
    } else if (renderer == threeDContainer) {
        threeDWindow->setFrame(
                frame.visual, frame.watts, frame.targetWatts,
                frame.cadenceRpm, frame.heartRate, frame.virtualGear);
    }
}
