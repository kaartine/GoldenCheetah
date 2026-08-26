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
#include <QQuickItem>
#include <QDebug>
#include <QStringList>
#include <QTextStream>
#include <QUrl>

#include <chrono>

namespace {

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

}

WorkoutGame3DWindow::WorkoutGame3DWindow(
        bool rendererEnabled,
        QWindow *parent) :
    QQuickView(parent),
    viewModel(new WorkoutGame3DViewModel(this))
{
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
        QMetaObject::invokeMethod(
                this,
                [this, presentationTimeNs]() {
                    handlePresentedFrame(presentationTimeNs);
                },
                Qt::QueuedConnection);
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
           << " camera_pos="
                << viewModel->cameraX() << ','
                << viewModel->cameraY() << ','
                << viewModel->cameraZ()
           << " camera_target="
                << viewModel->cameraTargetX() << ','
                << viewModel->cameraTargetY() << ','
                << viewModel->cameraTargetZ()
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
