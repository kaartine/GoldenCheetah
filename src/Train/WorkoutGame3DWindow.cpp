/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DWindow.h"

#include <QCoreApplication>
#include <QEvent>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QDebug>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QUrl>

#include <chrono>

namespace {

constexpr std::int64_t FrameTimingWarmupMilliseconds = 500;

QEvent::Type presentationEventType()
{
    static const int type = QEvent::registerEventType();
    return static_cast<QEvent::Type>(type);
}

const char *featurePhaseName(WorkoutGameFeaturePhase phase)
{
    switch (phase) {
    case WorkoutGameFeaturePhase::Approach: return "approach";
    case WorkoutGameFeaturePhase::Measure: return "measure";
    case WorkoutGameFeaturePhase::Committed: return "committed";
    case WorkoutGameFeaturePhase::Action: return "action";
    case WorkoutGameFeaturePhase::Recovery: return "recovery";
    case WorkoutGameFeaturePhase::None: return "none";
    }
    return "none";
}

const char *routeName(WorkoutGameRoute route)
{
    return route == WorkoutGameRoute::SafeBypass ? "bypass" : "main";
}

const char *featureOutcomeName(WorkoutGameFeatureOutcome outcome)
{
    switch (outcome) {
    case WorkoutGameFeatureOutcome::None: return "none";
    case WorkoutGameFeatureOutcome::Active: return "active";
    case WorkoutGameFeatureOutcome::Completed: return "completed";
    case WorkoutGameFeatureOutcome::Bypassed: return "bypassed";
    }
    return "none";
}

const char *terrainName(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::SmoothTrail: return "smooth-trail";
    case WorkoutGameTerrainKind::Roots: return "roots";
    case WorkoutGameTerrainKind::Rollers: return "rollers";
    case WorkoutGameTerrainKind::Climb: return "climb";
    case WorkoutGameTerrainKind::RockGarden: return "rock-garden";
    case WorkoutGameTerrainKind::BunnyHop: return "bunny-hop";
    case WorkoutGameTerrainKind::Drop: return "drop";
    case WorkoutGameTerrainKind::Skinny: return "skinny";
    case WorkoutGameTerrainKind::Berm: return "berm";
    case WorkoutGameTerrainKind::LogOver: return "log-over";
    case WorkoutGameTerrainKind::Tabletop: return "tabletop";
    case WorkoutGameTerrainKind::RockSlab: return "rock-slab";
    case WorkoutGameTerrainKind::GapJump: return "gap-jump";
    }
    return "smooth-trail";
}

}

WorkoutGame3DWindow::WorkoutGame3DWindow(
        bool rendererEnabled,
        QWindow *parent) :
    QQuickView(parent),
    viewModel(new WorkoutGame3DViewModel(this))
{
    QSurfaceFormat surfaceFormat = format();
    surfaceFormat.setSwapInterval(0);
    setFormat(surfaceFormat);
    diagnosticsEnabled = qEnvironmentVariableIntValue(
            "GC_WORKOUT_GAME_DIAGNOSTICS") != 0;
    traceEnabled = qEnvironmentVariableIntValue(
            "GC_WORKOUT_GAME_TRACE") != 0;
    setResizeMode(QQuickView::SizeRootObjectToView);
    setColor(QColor(105, 154, 184));
    rootContext()->setContextProperty(
            QStringLiteral("workoutGame3D"), viewModel);
    connect(this, &QQuickView::statusChanged,
            this, &WorkoutGame3DWindow::handleStatusChanged);
    connect(this, &QQuickWindow::frameSwapped, this, [this]() {
        const std::int64_t presentationTimeNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                .count();
        pendingPresentationTimeNs.store(
                presentationTimeNs, std::memory_order_release);
        if (!presentationDispatchPending.exchange(
                    true, std::memory_order_acq_rel)) {
            QCoreApplication::postEvent(
                    this, new QEvent(presentationEventType()),
                    Qt::LowEventPriority);
        }
    }, Qt::DirectConnection);
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

bool WorkoutGame3DWindow::event(QEvent *event)
{
    if (event->type() == presentationEventType()) {
        presentationDispatchPending.store(false, std::memory_order_release);
        const std::int64_t presentationTimeNs =
                pendingPresentationTimeNs.exchange(
                    0, std::memory_order_acq_rel);
        if (presentationTimeNs > 0) {
            handlePresentedFrame(presentationTimeNs);
        }
        return true;
    }
    return QQuickView::event(event);
}

void WorkoutGame3DWindow::setCourse(
        const WorkoutGameCourse &course,
        double ftpWatts)
{
    visualSmoother.reset();
    diagnostics.reset();
    publishedDiagnostics = {};
    viewModel->setDiagnostics(publishedDiagnostics);
    roadCourse = WorkoutGameRoadCourseBuilder::build(course, ftpWatts);
    sourceFrame = {};
    presentedFrame = {};
    frameNumber = 0;
    lastTracePublishMs = -1;
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
    sourceFrame = frame;
    watts = newWatts;
    targetWatts = newTargetWatts;
    cadenceRpm = newCadenceRpm;
    heartRate = newHeartRate;
    virtualGear = newVirtualGear;
    if (!sessionRunning) {
        visualSmoother.reset();
        hasFrame = false;
    }
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
    if (running != sessionRunning) {
        diagnostics.reset();
        publishedDiagnostics = {};
        viewModel->setDiagnostics(publishedDiagnostics);
        frameNumber = 0;
        lastTracePublishMs = -1;
    }
    sessionRunning = running;
    if (rootObject()) {
        rootObject()->setProperty("sessionRunning", sessionRunning);
    }
    if (running) {
        frameTimingWarmupStartMs = monotonicClock.elapsed();
        frameRateCounter.reset();
        presentFrame();
    }
}

void WorkoutGame3DWindow::setGeneratorState(const QString &state)
{
    viewModel->setGeneratorState(state);
}

void WorkoutGame3DWindow::presentFrame()
{
    if (!hasFrame) return;
    presentedFrame = visualSmoother.sample(monotonicClock.elapsed());
    viewModel->setFrame(
            presentedFrame,
            watts, targetWatts, cadenceRpm, heartRate, virtualGear);
}

void WorkoutGame3DWindow::handlePresentedFrame(
        std::int64_t presentationTimeNs)
{
    if (!sessionRunning) return;
    const double fps = frameRateCounter.frameRenderedNanoseconds(
            presentationTimeNs);
    viewModel->setFps(fps);
    QElapsedTimer presentationWork;
    presentationWork.start();
    presentFrame();
    updateDiagnostics(
            presentationTimeNs / 1000000,
            double(presentationWork.nsecsElapsed()) / 1000000.0);
}

void WorkoutGame3DWindow::updateDiagnostics(
        std::int64_t monotonicTimeMs,
        double presentationWorkMs)
{
    if ((!diagnosticsEnabled && !traceEnabled)
            || !sessionRunning || !hasFrame) {
        return;
    }

    const WorkoutGameRoadTimelineSample sourceTimeline =
            WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(
                roadCourse, sourceFrame.simulation.workoutTimeMs);
    const WorkoutGameRoadTimelineSample renderedTimeline =
            WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(
                roadCourse, presentedFrame.simulation.workoutTimeMs);
    WorkoutGameDiagnosticsInput input;
    input.ready = sourceTimeline.ready && renderedTimeline.ready;
    input.sessionRunning = sessionRunning;
    input.movingForward = presentedFrame.simulation.ready
            && !presentedFrame.simulation.finished
            && presentedFrame.simulation.speedKph > 0.2;
    input.frameTimingWarmupComplete = monotonicTimeMs
            - frameTimingWarmupStartMs >= FrameTimingWarmupMilliseconds;
    input.frameNumber = ++frameNumber;
    input.monotonicTimeMs = monotonicTimeMs;
    input.sourceWorkoutTimeMs = sourceFrame.simulation.workoutTimeMs;
    input.renderedWorkoutTimeMs =
            presentedFrame.simulation.workoutTimeMs;
    input.sourceSection = sourceTimeline.ready
            ? int(sourceTimeline.sourceSectionIndex) : -1;
    input.renderedSection = renderedTimeline.ready
            ? int(renderedTimeline.sourceSectionIndex) : -1;
    input.sourceSectionProgress = sourceTimeline.sectionProgress;
    input.renderedSectionProgress = renderedTimeline.sectionProgress;
    input.sourceRoadDistanceMeters = sourceTimeline.distanceMeters;
    input.renderedRoadDistanceMeters = renderedTimeline.distanceMeters;
    input.framesPerSecond = frameRateCounter.framesPerSecond();
    input.p50FrameIntervalMs =
            frameRateCounter.p50FrameIntervalMilliseconds();
    input.p95FrameIntervalMs =
            frameRateCounter.p95FrameIntervalMilliseconds();
    input.p99FrameIntervalMs =
            frameRateCounter.p99FrameIntervalMilliseconds();
    input.skippedSimulationTicks =
            presentedFrame.skippedSimulationTicks;
    input.rendererQueueDepth = viewModel->geometryQueueDepth();
    input.presentationWorkMs = presentationWorkMs;
    input.worldReady = presentedFrame.world.ready;
    input.riderAirborne = presentedFrame.world.rider.airborne;
    input.airborneExpected = WorkoutGameFeatureRuntime::airborneExpected(
            presentedFrame.feature);
    input.riderElevationMeters =
            presentedFrame.world.rider.elevationMeters;
    input.surfaceElevationMeters =
            presentedFrame.world.surfaceElevationMeters;
    input.riderClearanceMeters =
            presentedFrame.world.rider.clearanceMeters;
    input.airHeightMeters =
            presentedFrame.world.rider.airHeightMeters();
    input.lateralOffsetMeters =
            presentedFrame.feature.lateralOffsetMeters;
    input.visibleElevationChangeMeters =
            presentedFrame.world.rider.elevationMeters
            - presentedFrame.world.surfaceElevationMeters;
    input.renderedGradePercent = presentedFrame.world.gradePercent;
    publishedDiagnostics = diagnostics.update(input);
    viewModel->setDiagnostics(publishedDiagnostics);

    if (traceEnabled && publishedDiagnostics.ready
            && (lastTracePublishMs < 0
                || monotonicTimeMs - lastTracePublishMs >= 250)) {
        lastTracePublishMs = monotonicTimeMs;
        qInfo().noquote() << diagnosticsTraceLine();
    }
}

QString WorkoutGame3DWindow::diagnosticsTraceLine() const
{
    if (!publishedDiagnostics.ready) return QString();
    const WorkoutGameDiagnosticsInput &input = publishedDiagnostics.input;
    QString featureGeometry = viewModel->terrainName().toLower();
    featureGeometry.replace(QLatin1Char(' '), QLatin1Char('-'));
    QString result;
    QTextStream stream(&result);
    stream << "workout-game-3d-trace"
           << " frame=" << input.frameNumber
           << " mono_ms=" << input.monotonicTimeMs
           << " source_ms=" << input.sourceWorkoutTimeMs
           << " render_ms=" << input.renderedWorkoutTimeMs
           << " source_road_m=" << input.sourceRoadDistanceMeters
           << " render_road_m=" << input.renderedRoadDistanceMeters
           << " delta_m=" << publishedDiagnostics.frameDistanceDeltaMeters
           << " frame_ms=" << publishedDiagnostics.frameIntervalMs
           << " timing_warm="
                << int(input.frameTimingWarmupComplete)
           << " fps=" << input.framesPerSecond
           << " p50_frame_ms=" << input.p50FrameIntervalMs
           << " p95_frame_ms=" << input.p95FrameIntervalMs
           << " p99_frame_ms=" << input.p99FrameIntervalMs
           << " max_frame_ms="
                << publishedDiagnostics.largestFrameIntervalMs
           << " late_frames=" << publishedDiagnostics.lateFrameCount
           << " backwards=" << publishedDiagnostics.backwardFrameCount
           << " stationary=" << publishedDiagnostics.stationaryFrameCount
           << " skipped_ticks=" << input.skippedSimulationTicks
           << " geometry_queue=" << input.rendererQueueDepth
           << " presentation_work_ms=" << input.presentationWorkMs
           << " max_presentation_work_ms="
                << publishedDiagnostics.largestPresentationWorkMs
           << " long_presentation_work="
                << publishedDiagnostics.longPresentationWorkCount
           << " feature_phase="
                << featurePhaseName(presentedFrame.feature.phase)
           << " feature_outcome="
                << featureOutcomeName(presentedFrame.feature.outcome)
           << " feature_terrain="
                << terrainName(presentedFrame.feature.terrain)
           << " route=" << routeName(presentedFrame.feature.route)
           << " readiness=" << presentedFrame.feature.readiness
           << " action_distance_m="
                << presentedFrame.feature.distanceToObstacleMeters
           << " action_id=" << presentedFrame.feature.actionId
           << " rear_contact="
                << int(presentedFrame.world.rider.rearWheelGrounded)
           << " front_contact="
                << int(presentedFrame.world.rider.frontWheelGrounded)
           << " rear_suspension="
                << presentedFrame.world.rider.rearSuspension
           << " front_suspension="
                << presentedFrame.world.rider.frontSuspension
           << " airborne=" << int(presentedFrame.world.rider.airborne)
           << " lateral_m=" << input.lateralOffsetMeters
           << " unexpected_airborne_frames="
                << publishedDiagnostics.unexpectedAirborneFrameCount
           << " watts=" << watts
           << " target_watts=" << targetWatts
           << " cadence=" << cadenceRpm
           << " hr=" << heartRate
           << " gear=" << virtualGear
           << " speed_kph=" << presentedFrame.simulation.speedKph
           << " camera_pos="
                << viewModel->cameraX() << ','
                << viewModel->cameraY() << ','
                << viewModel->cameraZ()
           << " camera_target="
                << viewModel->cameraTargetX() << ','
                << viewModel->cameraTargetY() << ','
                << viewModel->cameraTargetZ()
           << " camera_presentation="
                << viewModel->cameraPresentation()
           << " camera_side_blend="
                << viewModel->cameraPresentationBlend()
           << " rider_asset=RB-01"
           << " surface_asset=TR-08"
           << " near_environment=EN-01"
           << " distant_environment=EN-03"
           << " feature_geometry=" << featureGeometry
           << " lod=resident"
           << " visible_triangles=" << viewModel->visibleTriangles();
    return result;
}

void WorkoutGame3DWindow::handleStatusChanged(QQuickView::Status status)
{
    if (status == QQuickView::Ready && rootObject()) {
        rootObject()->setProperty("sessionRunning", sessionRunning);
    }
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
