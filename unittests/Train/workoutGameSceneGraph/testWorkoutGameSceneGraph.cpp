/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameSceneGraphWindow.h"
#include "Train/WorkoutGameDiagnostics.h"
#include "Train/WorkoutGameFeatureLab.h"
#include "Train/WorkoutGameFeatureRuntime.h"
#include "Train/WorkoutGamePowerCueGeometry.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QImage>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSet>
#include <QTest>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>

namespace {

WorkoutGameCourse sampleCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 441u;
    course.durationMs = 90000;
    WorkoutGameSection trail;
    trail.feature = WorkoutGameFeature::Trail;
    trail.terrain = WorkoutGameTerrainKind::SmoothTrail;
    trail.durationMs = 30000;
    trail.targetWatts = 170.0;
    trail.difficulty = 0.3;
    WorkoutGameSection climb = trail;
    climb.feature = WorkoutGameFeature::Climb;
    climb.terrain = WorkoutGameTerrainKind::Climb;
    climb.startMs = 30000;
    climb.targetWatts = 230.0;
    climb.gradePercent = 7.0;
    climb.difficulty = 0.7;
    climb.visualVariant = 2u;
    WorkoutGameSection log = trail;
    log.feature = WorkoutGameFeature::SprintJump;
    log.terrain = WorkoutGameTerrainKind::LogOver;
    log.startMs = 60000;
    log.targetWatts = 270.0;
    log.challengeCount = 1;
    log.difficulty = 0.6;
    log.visualVariant = 3u;
    course.sections = {trail, climb, log};
    return course;
}

WorkoutGameVisualSnapshot frameAt(double distanceMeters)
{
    WorkoutGameVisualSnapshot frame;
    frame.simulation.ready = true;
    frame.simulation.workoutTimeMs = std::int64_t(distanceMeters * 200.0);
    frame.simulation.courseProgress = double(frame.simulation.workoutTimeMs)
            / 90000.0;
    frame.simulation.sectionProgress = std::clamp(
            double(frame.simulation.workoutTimeMs - 30000) / 30000.0,
            0.0, 1.0);
    frame.simulation.speedKph = 18.4;
    frame.simulation.activeSection = 1;
    frame.world.ready = true;
    frame.world.generation = 1;
    frame.world.terrain = WorkoutGameTerrainKind::Climb;
    frame.world.rider.distanceMeters = distanceMeters;
    frame.world.rider.rearSuspension = 0.15;
    frame.world.rider.frontSuspension = 0.2;
    frame.world.speedMetersPerSecond = 5.1;
    return frame;
}

WorkoutGameVisualSnapshot featureFrame(
        WorkoutGameFeatureOutcome outcome,
        WorkoutGameRoute route)
{
    WorkoutGameVisualSnapshot frame;
    frame.simulation.ready = true;
    frame.simulation.workoutTimeMs = 88200;
    frame.simulation.courseProgress = 0.94;
    frame.simulation.sectionProgress = 0.94;
    frame.simulation.speedKph = 20.0;
    frame.simulation.activeSection = 2;
    frame.simulation.featureOutcome = outcome;
    frame.simulation.route = route;
    frame.simulation.challengeReadiness = outcome
            == WorkoutGameFeatureOutcome::Completed ? 1.0 : 0.4;
    frame.world.ready = true;
    frame.world.generation = 1;
    frame.world.terrain = WorkoutGameTerrainKind::LogOver;
    frame.world.rider.distanceMeters = 1.0;
    frame.world.speedMetersPerSecond = 5.5;
    WorkoutGameFeatureRuntime runtime;
    if (!runtime.configure(WorkoutGameRoadCourseBuilder::build(
            sampleCourse(), 200.0))) {
        return frame;
    }
    frame.feature = runtime.update(frame.simulation);
    frame.world.rider.distanceMeters = frame.feature.visualDistanceMeters;
    return frame;
}

int changedPixels(const QImage &left, const QImage &right, int top)
{
    int changed = 0;
    for (int y = top; y < left.height(); y += 2) {
        for (int x = 0; x < left.width(); x += 2) {
            if (left.pixel(x, y) != right.pixel(x, y)) ++changed;
        }
    }
    return changed;
}

}

class TestWorkoutGameSceneGraph : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    }

    void rendersAndMovesPseudoThreeDimensionalRoad()
    {
        WorkoutGameSceneGraphWindow window;
        window.resize(1280, 720);
        window.setCourse(sampleCourse(), 200.0);
        window.setFrame(frameAt(200.0), 215.0, 230.0, 86, 151, 5);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
        QTest::qWait(1200);

        const QImage first = window.grabWindow();
        QVERIFY(!first.isNull());
        QCOMPARE(first.size(), QSize(1280, 720));
        QSet<QRgb> colors;
        for (int y = 0; y < first.height(); y += 12) {
            for (int x = 0; x < first.width(); x += 12) {
                colors.insert(first.pixel(x, y));
            }
        }
        QVERIFY(colors.size() > 120);
        QVERIFY(QColor(first.pixel(640, 690))
                != QColor(first.pixel(60, 690)));

        window.setFrame(frameAt(212.0), 245.0, 230.0, 91, 154, 6);
        QTest::qWait(300);
        const QImage second = window.grabWindow();
        QVERIFY(!second.isNull());
        const int roadChanges = changedPixels(first, second, 250);
        QVERIFY2(roadChanges > 1200,
                 qPrintable(QStringLiteral("only %1 road pixels changed")
                         .arg(roadChanges)));

        const QString screenshotPath = QDir(QDir::tempPath()).filePath(
                QStringLiteral("workout-game-scenegraph-test.png"));
        QVERIFY(second.save(screenshotPath));
    }

    void featureLabVisualDistanceNeverMovesBackward()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(course, 200.0));

        WorkoutGameVisualSmoother smoother;
        double previousDistance = -1.0;
        for (std::int64_t sourceTimeMs = 0;
             sourceTimeMs <= course.durationMs; sourceTimeMs += 1000) {
            const WorkoutGameSimulationSnapshot simulationFrame =
                    simulation.update(WorkoutGameFeatureLab::input(
                            course, sourceTimeMs,
                            WorkoutGameFeatureLabScenario::Pass));
            WorkoutGameVisualSnapshot frame;
            frame.simulation = simulationFrame;
            smoother.setTarget(frame, sourceTimeMs);

            for (std::int64_t renderTimeMs = sourceTimeMs;
                 renderTimeMs < sourceTimeMs + 1000;
                 renderTimeMs += 16) {
                const WorkoutGameVisualSnapshot rendered =
                        smoother.sample(renderTimeMs);
                const WorkoutGameRoadTimelineSample timeline =
                        WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(
                                road, rendered.simulation.workoutTimeMs);
                if (!timeline.ready) continue;
                QVERIFY2(
                        timeline.distanceMeters + 1e-9 >= previousDistance,
                        qPrintable(QStringLiteral(
                                "road moved backward at source=%1 render=%2: "
                                "%3 -> %4")
                                .arg(sourceTimeMs)
                                .arg(renderTimeMs)
                                .arg(previousDistance, 0, 'f', 6)
                                .arg(timeline.distanceMeters, 0, 'f', 6)));
                previousDistance = timeline.distanceMeters;
            }
        }
    }

    void diagnosticsDetectsBackwardAndStationaryFrames()
    {
        WorkoutGameDiagnostics diagnostics;
        WorkoutGameDiagnosticsInput input;
        input.ready = true;
        input.sessionRunning = true;
        input.movingForward = true;
        input.renderedWorkoutTimeMs = 1000;
        input.renderedRoadDistanceMeters = 10.0;
        input.framesPerSecond = 57.4;
        input.p95FrameIntervalMs = 21.0;
        input.skippedSimulationTicks = 2;
        const WorkoutGameDiagnosticsSnapshot first = diagnostics.update(input);
        QVERIFY(first.ready);
        QCOMPARE(first.input.framesPerSecond, 57.4);
        QCOMPARE(first.input.p95FrameIntervalMs, 21.0);
        QCOMPARE(first.input.skippedSimulationTicks, std::size_t(2));

        input.renderedWorkoutTimeMs = 1016;
        input.monotonicTimeMs = 16;
        input.renderedRoadDistanceMeters = 10.0;
        QCOMPARE(diagnostics.update(input).stationaryFrameCount,
                 std::uint64_t(1));

        input.renderedWorkoutTimeMs = 1032;
        input.monotonicTimeMs = 48;
        input.renderedRoadDistanceMeters = 9.75;
        const WorkoutGameDiagnosticsSnapshot regression =
                diagnostics.update(input);
        QCOMPARE(regression.backwardFrameCount, std::uint64_t(1));
        QCOMPARE(regression.largestRegressionMeters, 0.25);
        QCOMPARE(regression.frameIntervalMs, std::int64_t(32));
        QCOMPARE(regression.lateFrameCount, std::uint64_t(1));

        input.renderedWorkoutTimeMs = 0;
        input.renderedRoadDistanceMeters = 0.0;
        const WorkoutGameDiagnosticsSnapshot reset = diagnostics.update(input);
        QCOMPARE(reset.backwardFrameCount, std::uint64_t(0));
        QCOMPARE(reset.lateFrameCount, std::uint64_t(0));
    }

    void diagnosticsIgnoreInactivePresentationGaps()
    {
        WorkoutGameDiagnostics diagnostics;
        WorkoutGameDiagnosticsInput input;
        input.ready = true;
        input.sessionRunning = true;
        input.movingForward = true;
        input.monotonicTimeMs = 1000;
        input.renderedWorkoutTimeMs = 1000;
        input.renderedRoadDistanceMeters = 10.0;
        QVERIFY(diagnostics.update(input).ready);

        input.monotonicTimeMs = 1020;
        input.renderedWorkoutTimeMs = 1020;
        input.renderedRoadDistanceMeters = 10.2;
        QCOMPARE(diagnostics.update(input).frameIntervalMs,
                 std::int64_t(20));

        input.sessionRunning = false;
        input.monotonicTimeMs = 9000;
        QVERIFY(!diagnostics.update(input).ready);

        input.sessionRunning = true;
        input.monotonicTimeMs = 9020;
        input.renderedWorkoutTimeMs = 1040;
        input.renderedRoadDistanceMeters = 10.4;
        const WorkoutGameDiagnosticsSnapshot resumed =
                diagnostics.update(input);
        QVERIFY(resumed.ready);
        QCOMPARE(resumed.frameIntervalMs, std::int64_t(0));
        QCOMPARE(resumed.largestFrameIntervalMs, std::int64_t(0));
        QCOMPARE(resumed.backwardFrameCount, std::uint64_t(0));
    }

    void powerCueBandsAreShortAndStayInsideTheTrail()
    {
        WorkoutGameFeatureRuntimeSnapshot feature;
        feature.ready = true;
        feature.phase = WorkoutGameFeaturePhase::Measure;
        feature.prepareDistanceMeters = 20.0;
        feature.decisionDistanceMeters = 170.0;
        feature.visualDistanceMeters = 155.0;

        const std::vector<WorkoutGamePowerCueBand> bands =
                WorkoutGamePowerCueGeometry::build(
                    feature, WorkoutGameChallengeCue::CarrySpeed);

        QVERIFY(!bands.empty());
        QVERIFY(bands.size() <= std::size_t(7));
        for (const WorkoutGamePowerCueBand &band : bands) {
            QVERIFY(band.startDistanceMeters >= 160.0);
            QVERIFY(band.endDistanceMeters <= 170.4);
            QVERIFY(band.endDistanceMeters > band.startDistanceMeters);
            QVERIFY(band.halfWidthRatio > 0.0);
            QVERIFY(band.halfWidthRatio <= 0.9);
        }
    }

    void climbUsesHudInsteadOfPaintingTheWholeRoadYellow()
    {
        WorkoutGameFeatureRuntimeSnapshot feature;
        feature.ready = true;
        feature.phase = WorkoutGameFeaturePhase::Measure;
        feature.prepareDistanceMeters = 0.0;
        feature.decisionDistanceMeters = 900.0;
        feature.visualDistanceMeters = 400.0;

        QVERIFY(WorkoutGamePowerCueGeometry::build(
                    feature, WorkoutGameChallengeCue::Climb).empty());
    }

    void diagnosticsCountOnlyUnexpectedAirTime()
    {
        WorkoutGameDiagnostics diagnostics;
        WorkoutGameDiagnosticsInput input;
        input.ready = true;
        input.sessionRunning = true;
        input.movingForward = true;
        input.worldReady = true;
        input.monotonicTimeMs = 1000;
        input.renderedWorkoutTimeMs = 1000;
        input.renderedRoadDistanceMeters = 10.0;
        input.riderAirborne = true;
        input.airborneExpected = false;
        input.riderClearanceMeters = 0.85;
        input.airHeightMeters = 0.03;
        QCOMPARE(diagnostics.update(input).unexpectedAirborneFrameCount,
                 std::uint64_t(0));

        input.monotonicTimeMs = 1020;
        input.renderedWorkoutTimeMs = 1020;
        input.renderedRoadDistanceMeters = 10.1;
        QCOMPARE(diagnostics.update(input).unexpectedAirborneFrameCount,
                 std::uint64_t(0));

        input.monotonicTimeMs = 1040;
        input.renderedWorkoutTimeMs = 1040;
        input.renderedRoadDistanceMeters = 10.2;
        input.riderClearanceMeters = 1.15;
        input.airHeightMeters = 0.33;
        QCOMPARE(diagnostics.update(input).unexpectedAirborneFrameCount,
                 std::uint64_t(1));

        input.monotonicTimeMs = 1060;
        input.renderedWorkoutTimeMs = 1060;
        input.renderedRoadDistanceMeters = 10.3;
        input.airborneExpected = true;
        QCOMPARE(diagnostics.update(input).unexpectedAirborneFrameCount,
                 std::uint64_t(1));
    }

    void completedAndBypassedFeaturesRenderDifferentLines()
    {
        WorkoutGameSceneGraphWindow window;
        window.resize(1280, 720);
        window.setCourse(sampleCourse(), 200.0);
        window.setFrame(
                featureFrame(WorkoutGameFeatureOutcome::Completed,
                             WorkoutGameRoute::MainLine),
                270.0, 270.0, 88, 155, 7);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
        QTest::qWait(400);
        const QImage completed = window.grabWindow();
        QVERIFY(!completed.isNull());

        window.setFrame(
                featureFrame(WorkoutGameFeatureOutcome::Bypassed,
                             WorkoutGameRoute::SafeBypass),
                110.0, 270.0, 45, 130, 2);
        QTest::qWait(400);
        const QImage bypassed = window.grabWindow();
        QVERIFY(!bypassed.isNull());

        QVERIFY(changedPixels(completed, bypassed, 180) > 1800);
    }

    void featureLabRendersFiveDistinctObstacles()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        WorkoutGameFeatureRuntime featureRuntime;
        QVERIFY(featureRuntime.configure(road));
        WorkoutGameSceneGraphWindow window;
        window.resize(1280, 720);
        window.setCourse(course, 200.0);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);

        QImage prior;
        const int featureSections[] = {0, 2, 4, 6, 8};
        for (int section : featureSections) {
            const WorkoutGameRoadPiece *challengePiece = nullptr;
            for (const WorkoutGameRoadPiece &piece : road.pieces) {
                if (piece.sourceSectionIndex == std::size_t(section)
                        && piece.challenge.enabled) {
                    challengePiece = &piece;
                    break;
                }
            }
            QVERIFY(challengePiece);
            const WorkoutGameRoadTimelineSection &timeline =
                    road.timeline[std::size_t(section)];
            const double sectionLength = timeline.endDistanceMeters
                    - timeline.startDistanceMeters;
            const double visualDistance = std::max(
                    timeline.startDistanceMeters,
                    challengePiece->challenge.obstacleDistanceMeters - 8.0);
            const double sectionProgress = std::clamp(
                    (visualDistance - timeline.startDistanceMeters)
                        / sectionLength,
                    0.0, 1.0);
            WorkoutGameVisualSnapshot frame;
            frame.simulation.ready = true;
            frame.simulation.workoutTimeMs =
                    course.sections[std::size_t(section)].startMs
                    + std::int64_t(std::llround(
                        course.sections[std::size_t(section)].durationMs
                            * sectionProgress));
            frame.simulation.activeSection = section;
            frame.simulation.sectionProgress = sectionProgress;
            frame.simulation.courseProgress = double(frame.simulation.workoutTimeMs)
                    / double(course.durationMs);
            frame.simulation.speedKph = 20.0;
            frame.simulation.featureOutcome = WorkoutGameFeatureOutcome::Completed;
            frame.simulation.challengeReadiness = 1.0;
            frame.world.ready = true;
            frame.world.generation = std::uint64_t(section + 1);
            frame.world.terrain = course.sections[std::size_t(section)].terrain;
            frame.world.speedMetersPerSecond = 5.5;
            frame.feature = featureRuntime.update(frame.simulation);
            frame.world.rider.distanceMeters =
                    frame.feature.visualDistanceMeters;
            window.setFrame(frame, 220.0,
                    course.sections[std::size_t(section)].targetWatts,
                    88, 150, 7);
            QTest::qWait(300);
            const QImage rendered = window.grabWindow();
            QVERIFY(!rendered.isNull());
            if (!prior.isNull()) {
                const int featureChanges = changedPixels(prior, rendered, 180);
                QVERIFY2(featureChanges > 900,
                         qPrintable(QStringLiteral(
                             "only %1 feature pixels changed for section %2")
                             .arg(featureChanges).arg(section)));
            }
            const QString output = QDir(QDir::tempPath()).filePath(
                    QStringLiteral("workout-game-feature-%1.png")
                        .arg(section));
            QVERIFY(rendered.save(output));
            prior = rendered;
        }
    }

    void capturesSceneFramesDirectly()
    {
        QTemporaryDir captures;
        QVERIFY(captures.isValid());
        qputenv("GC_WORKOUT_GAME_CAPTURE_DIR",
                captures.path().toLocal8Bit());
        qputenv("GC_WORKOUT_GAME_CAPTURE_MS", "50");
        qputenv("GC_WORKOUT_GAME_CAPTURE_FRAMES", "40");
        QString beforeUpdateFrame;
        {
            WorkoutGameSceneGraphWindow window;
            window.resize(1280, 720);
            window.setSessionRunning(true);
            window.setCourse(sampleCourse(), 200.0);
            window.setFrame(frameAt(200.0), 215.0, 230.0, 86, 151, 5);
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
            QTRY_VERIFY_WITH_TIMEOUT(
                    QDir(captures.path()).entryList(
                            {QStringLiteral("frame-*.png")},
                            QDir::Files, QDir::Name).size() >= 3,
                    3000);
            const QStringList beforeFrames = QDir(captures.path()).entryList(
                    {QStringLiteral("frame-*.png")}, QDir::Files, QDir::Name);
            beforeUpdateFrame = beforeFrames.back();
            const int beforeCount = beforeFrames.size();
            window.setFrame(frameAt(220.0), 245.0, 230.0, 91, 154, 6);
            QTRY_VERIFY_WITH_TIMEOUT(
                    QDir(captures.path()).entryList(
                            {QStringLiteral("frame-*.png")},
                            QDir::Files, QDir::Name).size() >= beforeCount + 5,
                    3000);
        }
        qunsetenv("GC_WORKOUT_GAME_CAPTURE_DIR");
        qunsetenv("GC_WORKOUT_GAME_CAPTURE_MS");
        qunsetenv("GC_WORKOUT_GAME_CAPTURE_FRAMES");
        const QStringList frames = QDir(captures.path()).entryList(
                {QStringLiteral("frame-*.png")}, QDir::Files, QDir::Name);
        QVERIFY2(frames.size() >= 4,
                 qPrintable(QStringLiteral("captured only %1 frames")
                         .arg(frames.size())));
        const QImage first(QDir(captures.path()).filePath(beforeUpdateFrame));
        const QImage last(QDir(captures.path()).filePath(frames.back()));
        QVERIFY(!first.isNull());
        QVERIFY(!last.isNull());
        const int captureChanges = changedPixels(first, last, 250);
        QVERIFY2(captureChanges > 900,
                 qPrintable(QStringLiteral("only %1 capture pixels changed")
                         .arg(captureChanges)));
    }

    void frameRateComesFromPresentedFrames()
    {
        qputenv("GC_WORKOUT_GAME_DIAGNOSTICS", "1");
        {
            WorkoutGameSceneGraphWindow window;
            window.resize(960, 540);
            window.setSessionRunning(true);
            window.setCourse(sampleCourse(), 200.0);
            window.setFrame(frameAt(200.0), 215.0, 230.0, 86, 151, 5);
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
            QTRY_VERIFY_WITH_TIMEOUT(
                    window.diagnosticsSnapshot().ready
                    && window.diagnosticsSnapshot().input.framesPerSecond > 1.0
                    && window.diagnosticsSnapshot().input
                        .p95FrameIntervalMs > 0.0,
                    3000);
            QVERIFY(window.diagnosticsSnapshot().input.frameNumber > 1);
        }
        qunsetenv("GC_WORKOUT_GAME_DIAGNOSTICS");
    }

    void chaseRiderSpriteSheetHasEightConsistentFrames()
    {
        const QImage sheet(QStringLiteral(
                ":/images/workout-game-rider-chase-sheet.png"));
        QVERIFY(!sheet.isNull());
        QCOMPARE(sheet.width() % 4, 0);
        QCOMPARE(sheet.height() % 2, 0);
        const int frameWidth = sheet.width() / 4;
        const int frameHeight = sheet.height() / 2;
        QVERIFY(frameWidth > 0);
        QVERIFY(frameHeight > frameWidth);
        for (int frame = 0; frame < 8; ++frame) {
            const QImage image = sheet.copy(
                    (frame % 4) * frameWidth,
                    (frame / 4) * frameHeight,
                    frameWidth, frameHeight);
            int opaquePixels = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (qAlpha(image.pixel(x, y)) >= 220) ++opaquePixels;
                }
            }
            QVERIFY2(opaquePixels > frameWidth * frameHeight / 12,
                     "rider animation frame has insufficient visible content");
            QVERIFY2(opaquePixels < frameWidth * frameHeight / 2,
                     "rider animation frame retained an opaque background");
        }
    }
};

QTEST_MAIN(TestWorkoutGameSceneGraph)
#include "testWorkoutGameSceneGraph.moc"
