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
#include <cmath>

namespace {

QEvent::Type presentationEventType()
{
    static const int type = QEvent::registerEventType();
    return static_cast<QEvent::Type>(type);
}

bool visuallyDifferent(
        const WorkoutGameVisualSnapshot &left,
        const WorkoutGameVisualSnapshot &right)
{
    const auto changed = [](double first, double second) {
        return !std::isfinite(first) || !std::isfinite(second)
                || std::abs(first - second) > 1.0e-9;
    };
    return left.sessionGeneration != right.sessionGeneration
            || left.simulation.ready != right.simulation.ready
            || left.simulation.finished != right.simulation.finished
            || left.simulation.workoutTimeMs != right.simulation.workoutTimeMs
            || left.world.ready != right.world.ready
            || left.world.generation != right.world.generation
            || changed(left.world.rider.distanceMeters,
                       right.world.rider.distanceMeters)
            || changed(left.world.rider.elevationMeters,
                       right.world.rider.elevationMeters)
            || changed(left.world.rider.pitchDegrees,
                       right.world.rider.pitchDegrees)
            || changed(left.world.rider.rollDegrees,
                       right.world.rider.rollDegrees)
            || changed(left.world.rider.rearWheelRadians,
                       right.world.rider.rearWheelRadians)
            || changed(left.world.rider.frontWheelRadians,
                       right.world.rider.frontWheelRadians)
            || changed(left.riderPedalCycles, right.riderPedalCycles)
            || left.camera.ready != right.camera.ready
            || changed(left.camera.centerDistanceMeters,
                       right.camera.centerDistanceMeters)
            || changed(left.camera.centerElevationMeters,
                       right.camera.centerElevationMeters)
            || changed(left.camera.yawDegrees, right.camera.yawDegrees)
            || changed(left.camera.pitchDegrees, right.camera.pitchDegrees)
            || left.feature.phase != right.feature.phase
            || left.feature.route != right.feature.route
            || left.feature.outcome != right.feature.outcome
            || changed(left.feature.lateralOffsetMeters,
                       right.feature.lateralOffsetMeters)
            || changed(left.feature.verticalOffsetMeters,
                       right.feature.verticalOffsetMeters);
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

const char *gapLineName(WorkoutGameGapJumpLine line)
{
    switch (line) {
    case WorkoutGameGapJumpLine::None: return "none";
    case WorkoutGameGapJumpLine::Short: return "short";
    case WorkoutGameGapJumpLine::Medium: return "medium";
    case WorkoutGameGapJumpLine::Long: return "long";
    }
    return "none";
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
        coldStartFrameCapture.recordFrame(
                presentationTimeNs,
                presentedVisualRevision.load(std::memory_order_acquire));
        presentedFrameSequence.fetch_add(1, std::memory_order_acq_rel);
        pendingPresentationTimeNs.store(
                presentationTimeNs, std::memory_order_release);
        if (!presentationDispatchPending.exchange(
                    true, std::memory_order_acq_rel)) {
            QCoreApplication::postEvent(
                    this, new QEvent(presentationEventType()),
                    Qt::NormalEventPriority);
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
        const std::int64_t presentationTimeNs =
                pendingPresentationTimeNs.exchange(
                    0, std::memory_order_acq_rel);
        presentationDispatchPending.store(false, std::memory_order_release);

        // A swap can land after the timestamp exchange but before the pending
        // flag is released. Re-arm the coalesced event in that window.
        if (pendingPresentationTimeNs.load(std::memory_order_acquire) > 0
                && !presentationDispatchPending.exchange(
                    true, std::memory_order_acq_rel)) {
            QCoreApplication::postEvent(
                    this, new QEvent(presentationEventType()),
                    Qt::NormalEventPriority);
        }

        const std::uint64_t presentedFrames =
                presentedFrameSequence.load(std::memory_order_acquire);
        const std::uint64_t prewarmStart =
                prewarmStartSequence.load(std::memory_order_acquire);
        if (prewarmPending.load(std::memory_order_acquire)
                && presentedFrames - prewarmStart
                    >= RendererPrewarmFrameCount) {
            finishRendererPrewarm();
        } else if (prewarmPending.load(std::memory_order_acquire)) {
            requestUpdate();
        }
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
    currentDiagnostics = {};
    publishedDiagnostics = {};
    viewModel->setDiagnostics(publishedDiagnostics);
    roadCourse = WorkoutGameRoadCourseBuilder::build(course, ftpWatts);
    sourceFrame = {};
    presentedFrame = {};
    hasPresentedVisualState = false;
    frameNumber = 0;
    lastTracePublishMs = -1;
    lastFpsPublishMs = -1;
    coldStartCompletePublished = false;
    hasFrame = false;
    viewModel->setCourse(course, ftpWatts);
    requestRendererPrewarm();
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
    const bool starting = running && !sessionRunning;
    const bool stopping = !running && sessionRunning;
    if (starting) {
        const std::int64_t startTimeNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                .count();
        coldStartFrameCapture.start(
                startTimeNs,
                presentedVisualRevision.load(std::memory_order_acquire));
    } else if (stopping) {
        coldStartFrameCapture.stop();
    }
    if (running != sessionRunning) {
        diagnostics.reset();
        currentDiagnostics = {};
        publishedDiagnostics = {};
        viewModel->setDiagnostics(publishedDiagnostics);
        frameNumber = 0;
        lastTracePublishMs = -1;
        lastFpsPublishMs = -1;
        coldStartCompletePublished = false;
    }
    sessionRunning = running;
    sessionRunningAtomic.store(running, std::memory_order_release);
    if (rootObject()) {
        rootObject()->setProperty("sessionRunning", sessionRunning);
    }
    if (running) {
        prewarmPending.store(false, std::memory_order_release);
        if (rootObject()) rootObject()->setProperty("rendererPrewarming", false);
        frameRateCounter.reset();
        presentFrame();
        requestUpdate();
    } else {
        coldStartFrameCapture.stop();
    }
}

void WorkoutGame3DWindow::setGeneratorState(const QString &state)
{
    viewModel->setGeneratorState(state);
}

void WorkoutGame3DWindow::presentFrame()
{
    if (!hasFrame) return;
    const WorkoutGameVisualSnapshot nextFrame =
            visualSmoother.sample(monotonicClock.elapsed());
    const bool changed = !hasPresentedVisualState
            || visuallyDifferent(presentedFrame, nextFrame);
    presentedFrame = nextFrame;
    hasPresentedVisualState = true;
    viewModel->setFrame(
            presentedFrame,
            watts, targetWatts, cadenceRpm, heartRate, virtualGear);
    if (changed) {
        presentedVisualRevision.fetch_add(1, std::memory_order_release);
    }
}

void WorkoutGame3DWindow::handlePresentedFrame(
        std::int64_t presentationTimeNs)
{
    if (!sessionRunning) return;
    frameRateCounter.frameRenderedNanoseconds(presentationTimeNs);
    const std::int64_t presentationTimeMs = presentationTimeNs / 1000000;
    if (lastFpsPublishMs < 0
            || presentationTimeMs - lastFpsPublishMs >= 250) {
        lastFpsPublishMs = presentationTimeMs;
        viewModel->setFps(frameRateCounter.framesPerSecond());
    }
    QElapsedTimer presentationWork;
    presentationWork.start();
    presentFrame();
    updateDiagnostics(
            presentationTimeNs,
            double(presentationWork.nsecsElapsed()) / 1000000.0);
}

void WorkoutGame3DWindow::updateDiagnostics(
        std::int64_t presentationTimeNs,
        double presentationWorkMs)
{
    if ((!diagnosticsEnabled && !traceEnabled)
            || !sessionRunning || !hasFrame) {
        return;
    }

    const std::int64_t monotonicTimeMs = presentationTimeNs / 1000000;
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
    input.frameTimingWarmupComplete = true;
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
    input.coldStart = coldStartFrameCapture.snapshot(presentationTimeNs);
    if (input.coldStart.frameCount > 1) {
        input.framesPerSecond = input.coldStart.swapFramesPerSecond;
        input.p99FrameIntervalMs = input.coldStart.p99FrameIntervalMs;
    }
    currentDiagnostics = diagnostics.update(input);

    const bool publishColdStartCompletion = input.coldStart.complete
            && !coldStartCompletePublished;
    if (lastTracePublishMs >= 0
            && monotonicTimeMs - lastTracePublishMs < 250
            && !publishColdStartCompletion) {
        return;
    }
    lastTracePublishMs = monotonicTimeMs;
    coldStartCompletePublished = coldStartCompletePublished
            || input.coldStart.complete;
    publishedDiagnostics = currentDiagnostics;
    viewModel->setFps(input.framesPerSecond);
    viewModel->setDiagnostics(publishedDiagnostics);
    if (traceEnabled && publishedDiagnostics.ready) {
        qInfo().noquote() << diagnosticsTraceLine();
    }
}

void WorkoutGame3DWindow::requestRendererPrewarm()
{
    if (sessionRunningAtomic.load(std::memory_order_acquire)
            || !rootObject()) {
        return;
    }
    prewarmCompleted.store(false, std::memory_order_release);
    prewarmStartSequence.store(
            presentedFrameSequence.load(std::memory_order_acquire),
            std::memory_order_release);
    prewarmPending.store(true, std::memory_order_release);
    rootObject()->setProperty("rendererPrewarming", true);
    requestUpdate();
}

void WorkoutGame3DWindow::finishRendererPrewarm()
{
    if (!prewarmPending.exchange(false, std::memory_order_acq_rel)) return;
    if (rootObject()) rootObject()->setProperty("rendererPrewarming", false);
    prewarmCompleted.store(true, std::memory_order_release);
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
           << " cold_complete=" << int(input.coldStart.complete)
           << " cold_samples=" << input.coldStart.frameCount
           << " cold_dropped_frames="
                << input.coldStart.droppedFrameCount
           << " cold_swap_fps="
                << input.coldStart.swapFramesPerSecond
           << " cold_visual_fps="
                << input.coldStart.uniqueVisualFramesPerSecond
           << " cold_start_first_swap_ms="
                << input.coldStart.startToFirstSwapMs
           << " cold_p99_frame_ms="
                << input.coldStart.p99FrameIntervalMs
           << " cold_max_frame_ms="
                << input.coldStart.maximumFrameIntervalMs
           << " cold_consecutive_late="
                << input.coldStart.maximumConsecutiveLateFrames
           << " cold_visual_stall_ms="
                << input.coldStart.longestUnchangedVisualIntervalMs
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
           << " distance_to_lip_m="
                << presentedFrame.feature.distanceToObstacleMeters
           << " launch_window="
                << int(presentedFrame.feature.launchWindowActive)
           << " candidate_line="
                << gapLineName(presentedFrame.feature.provisionalGapLine)
           << " steering_line="
                << gapLineName(presentedFrame.feature.steeringGapLine)
           << " locked_line="
                << gapLineName(presentedFrame.feature.lockedGapLine)
           << " rolling_500ms_speed_mps="
                << presentedFrame.feature.launchRollingSpeedMetersPerSecond
           << " best_500ms_speed_mps="
                << presentedFrame.feature.launchBestSpeedMetersPerSecond
           << " power_hold_ms="
                << presentedFrame.feature.launchPowerHoldMilliseconds
           << " line_reachable="
                << int(presentedFrame.feature.gapLineReachable)
           << " line_locked="
                << int(presentedFrame.feature.gapLineLocked)
           << " launch_speed_ready="
                << int(presentedFrame.feature.launchSpeedReady)
           << " launch_power_ready="
                << int(presentedFrame.feature.launchPowerReady)
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
        rootObject()->setProperty("rendererPrewarming", false);
        requestRendererPrewarm();
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
