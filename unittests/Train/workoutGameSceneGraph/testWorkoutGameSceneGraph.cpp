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
#include "Train/WorkoutGameFeaturePrompt.h"
#include "Train/WorkoutGameHorizon.h"
#include "Train/WorkoutGameMesh.h"
#include "Train/WorkoutGamePowerCueGeometry.h"
#include "Train/WorkoutGameRoadProjection.h"

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
#include <array>
#include <cmath>
#include <limits>

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
    const WorkoutGameCourse course = sampleCourse();
    const WorkoutGameRoadCourse road =
            WorkoutGameRoadCourseBuilder::build(course, 200.0);
    const auto piece = std::find_if(
            road.pieces.begin(), road.pieces.end(),
            [](const WorkoutGameRoadPiece &candidate) {
                return candidate.sourceSectionIndex == 2u
                        && candidate.challenge.enabled;
            });
    WorkoutGameVisualSnapshot frame;
    frame.simulation.ready = true;
    if (piece == road.pieces.end()) return frame;
    const WorkoutGameRoadTimelineSection &timeline = road.timeline[2];
    const double branchMiddle =
            (piece->challenge.decisionDistanceMeters
             + piece->challenge.bypassEndDistanceMeters) * 0.5;
    const double sectionProgress = (branchMiddle
            - timeline.startDistanceMeters)
            / (timeline.endDistanceMeters - timeline.startDistanceMeters);
    frame.simulation.workoutTimeMs = course.sections[2].startMs
            + std::int64_t(std::llround(
                sectionProgress * course.sections[2].durationMs));
    frame.simulation.courseProgress = double(frame.simulation.workoutTimeMs)
            / double(course.durationMs);
    frame.simulation.sectionProgress = sectionProgress;
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
    if (!runtime.configure(road)) {
        return frame;
    }
    frame.feature = runtime.update(frame.simulation);
    frame.world.rider.distanceMeters = frame.feature.visualDistanceMeters;
    return frame;
}

struct FeatureCatalogEntry
{
    WorkoutGameTerrainKind terrain;
    const char *name;
};

constexpr std::array<FeatureCatalogEntry, 11> FeatureCatalog = {{
    {WorkoutGameTerrainKind::Roots, "roots"},
    {WorkoutGameTerrainKind::Rollers, "rollers"},
    {WorkoutGameTerrainKind::Climb, "climb"},
    {WorkoutGameTerrainKind::RockGarden, "rock-garden"},
    {WorkoutGameTerrainKind::BunnyHop, "bunny-hop"},
    {WorkoutGameTerrainKind::Drop, "drop"},
    {WorkoutGameTerrainKind::Skinny, "skinny"},
    {WorkoutGameTerrainKind::Berm, "berm"},
    {WorkoutGameTerrainKind::LogOver, "log-over"},
    {WorkoutGameTerrainKind::Tabletop, "tabletop"},
    {WorkoutGameTerrainKind::RockSlab, "rock-slab"}
}};

WorkoutGameCourse catalogCourse(WorkoutGameTerrainKind terrain)
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 9173u + std::uint32_t(terrain);
    course.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = terrain == WorkoutGameTerrainKind::Climb
            ? WorkoutGameFeature::Climb
            : terrain == WorkoutGameTerrainKind::BunnyHop
                    || terrain == WorkoutGameTerrainKind::LogOver
                    || terrain == WorkoutGameTerrainKind::Tabletop
            ? WorkoutGameFeature::SprintJump
            : terrain == WorkoutGameTerrainKind::Drop
            ? WorkoutGameFeature::RecoveryDescent
            : WorkoutGameFeature::Trail;
    section.terrain = terrain;
    section.durationMs = course.durationMs;
    section.targetWatts = 220.0;
    section.gradePercent = terrain == WorkoutGameTerrainKind::Climb
            ? 8.0 : terrain == WorkoutGameTerrainKind::Drop ? -6.0 : 0.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    course.sections = {section};
    return course;
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

int exactColorPixels(const QImage &image, const QSet<QRgb> &colors)
{
    int count = 0;
    const QImage source = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = source.height() / 4; y < source.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(
                source.constScanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            if (colors.contains(line[x])) ++count;
        }
    }
    return count;
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
        QVERIFY(bands.size() <= std::size_t(5));
        for (const WorkoutGamePowerCueBand &band : bands) {
            QVERIFY(band.startDistanceMeters >= 163.0);
            QVERIFY(band.endDistanceMeters <= 170.4);
            QVERIFY(band.endDistanceMeters > band.startDistanceMeters);
            QVERIFY(band.halfWidthRatio > 0.0);
            QVERIFY(band.halfWidthRatio <= 0.9);
        }
    }

    void featurePromptStatesWhatToDoAndWhere()
    {
        WorkoutGamePowerProfileSnapshot profile;
        profile.ready = true;
        profile.cue.state = WorkoutGamePowerCueState::PushNow;
        profile.cue.challengeCue = WorkoutGameChallengeCue::Jump;
        profile.cue.readiness = 0.72;
        profile.cue.powerRequired = true;
        profile.cue.requiredWatts = 240.0;
        profile.cue.actualWatts = 180.0;
        WorkoutGameFeatureRuntimeSnapshot feature;
        feature.ready = true;
        feature.terrain = WorkoutGameTerrainKind::LogOver;
        feature.phase = WorkoutGameFeaturePhase::Measure;
        feature.distanceToObstacleMeters = 4.4;
        feature.motion = WorkoutGameFeatureMotion::Jump;

        const WorkoutGameFeaturePromptSnapshot pedal =
                WorkoutGameFeaturePrompt::build(profile, feature);
        QCOMPARE(pedal.instruction,
                 WorkoutGameFeatureInstruction::ReachTargetPower);
        QCOMPARE(pedal.terrain, WorkoutGameTerrainKind::LogOver);
        QCOMPARE(pedal.distanceMeters, 4.4);
        QCOMPARE(pedal.requiredWatts, 240.0);
        QCOMPARE(pedal.actualWatts, 180.0);

        profile.cue.readiness = 1.0;
        const WorkoutGameFeaturePromptSnapshot ready =
                WorkoutGameFeaturePrompt::build(profile, feature);
        QCOMPARE(ready.instruction,
                 WorkoutGameFeatureInstruction::Ready);

        feature.phase = WorkoutGameFeaturePhase::Action;
        const WorkoutGameFeaturePromptSnapshot takeoff =
                WorkoutGameFeaturePrompt::build(profile, feature);
        QCOMPARE(takeoff.instruction,
                 WorkoutGameFeatureInstruction::Takeoff);
    }

    void horizonHasClearlyVisibleForestRelief()
    {
        const WorkoutGameHorizonSnapshot horizon =
                WorkoutGameHorizon::build(441u, 250.0, 64);
        QVERIFY(horizon.ready);
        const auto far = std::minmax_element(
                horizon.farRidgeY.begin(), horizon.farRidgeY.end());
        const auto near = std::minmax_element(
                horizon.nearRidgeY.begin(), horizon.nearRidgeY.end());
        QVERIFY(*far.second - *far.first >= 0.09);
        QVERIFY(*near.second - *near.first >= 0.14);
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
                    challengePiece->challenge.obstacleDistanceMeters - 14.0);
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
            window.setCourse(course, 200.0);
            QTest::qWait(110);
            window.setFrame(frame, 220.0,
                    course.sections[std::size_t(section)].targetWatts,
                    88, 150, 7);
            QTest::qWait(300);
            const QImage rendered = window.grabWindow();
            QVERIFY(!rendered.isNull());
            QSet<QRgb> featureColors;
            if (section == 0 || section == 2) {
                featureColors.insert(qRgb(78, 47, 28));
                featureColors.insert(qRgb(166, 96, 43));
            } else if (section == 4) {
                featureColors.insert(qRgb(67, 73, 68));
                featureColors.insert(qRgb(145, 147, 130));
            } else if (section == 6) {
                featureColors.insert(qRgb(145, 105, 62));
                featureColors.insert(qRgb(79, 57, 39));
            } else {
                featureColors.insert(qRgb(45, 47, 43));
                featureColors.insert(qRgb(145, 147, 130));
            }
            const int visibleFeaturePixels = exactColorPixels(
                    rendered, featureColors);
            QVERIFY2(visibleFeaturePixels > 250,
                     qPrintable(QStringLiteral(
                         "only %1 visible obstacle pixels for section %2")
                         .arg(visibleFeaturePixels).arg(section)));
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

    void exportsEveryFeatureAtAConsistentViewpoint()
    {
        const QByteArray requestedOutput =
                qgetenv("GC_WORKOUT_GAME_FEATURE_CATALOG_DIR");
        const QString outputDirectory = requestedOutput.isEmpty()
                ? QDir(QDir::tempPath()).filePath(
                    QStringLiteral("workout-game-feature-catalog"))
                : QString::fromLocal8Bit(requestedOutput);
        QVERIFY(QDir().mkpath(outputDirectory));

        WorkoutGameSceneGraphWindow window;
        window.resize(1280, 720);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
        QImage prior;
        for (const FeatureCatalogEntry &entry : FeatureCatalog) {
            const WorkoutGameCourse course = catalogCourse(entry.terrain);
            const WorkoutGameRoadCourse road =
                    WorkoutGameRoadCourseBuilder::build(course, 200.0);
            QVERIFY(road.ready);
            const auto challenge = std::find_if(
                    road.pieces.begin(), road.pieces.end(),
                    [](const WorkoutGameRoadPiece &piece) {
                        return piece.challenge.enabled;
                    });
            QVERIFY(challenge != road.pieces.end());
            const WorkoutGameRoadTimelineSection &timeline = road.timeline.front();
            const double distance = std::max(
                    timeline.startDistanceMeters,
                    challenge->challenge.obstacleDistanceMeters - 10.0);
            WorkoutGameSimulationSnapshot simulation;
            simulation.ready = true;
            simulation.activeSection = 0;
            simulation.sectionProgress = std::clamp(
                    (distance - timeline.startDistanceMeters)
                        / (timeline.endDistanceMeters
                           - timeline.startDistanceMeters),
                    0.0, 1.0);
            simulation.workoutTimeMs = std::int64_t(std::llround(
                    course.durationMs * simulation.sectionProgress));
            simulation.courseProgress = simulation.sectionProgress;
            simulation.speedKph = 20.0;
            simulation.featureOutcome = WorkoutGameFeatureOutcome::Completed;
            simulation.route = WorkoutGameRoute::MainLine;
            simulation.challengeReadiness = 1.0;
            WorkoutGameFeatureRuntime runtime;
            QVERIFY(runtime.configure(road));
            WorkoutGameVisualSnapshot frame;
            frame.simulation = simulation;
            frame.feature = runtime.update(simulation);
            frame.world.ready = true;
            frame.world.generation = 1;
            frame.world.terrain = entry.terrain;
            frame.world.gradePercent = course.sections.front().gradePercent;
            frame.world.rider.distanceMeters = distance;
            frame.world.rider.clearanceMeters = 0.82;
            frame.world.speedMetersPerSecond = 20.0 / 3.6;

            window.setCourse(course, 200.0);
            window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
            QTest::qWait(260);
            const QImage rendered = window.grabWindow();
            QVERIFY(!rendered.isNull());
            QCOMPARE(rendered.size(), QSize(1280, 720));
            if (!prior.isNull()) {
                QVERIFY(changedPixels(prior, rendered, 180) > 700);
            }
            const QString output = QDir(outputDirectory).filePath(
                    QStringLiteral("feature-%1.png")
                        .arg(QString::fromLatin1(entry.name)));
            QVERIFY2(rendered.save(output), qPrintable(output));
            prior = rendered;
        }
    }

    void featureMeshesProjectAboveTheTrailAtEightMeters()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        for (const WorkoutGameRoadPiece &piece : road.pieces) {
            if (!piece.challenge.enabled) continue;
            const double riderDistance =
                    piece.challenge.obstacleDistanceMeters - 8.0;
            const WorkoutGameRoadProjectionFrame projection =
                    WorkoutGameRoadProjection::project(road, riderDistance);
            QVERIFY(projection.ready);
            WorkoutGameMeshInstance instance;
            instance.mesh = WorkoutGameMeshLibrary::feature(
                    piece.terrain, piece.difficulty);
            instance.anchorDistanceMeters =
                    piece.challenge.obstacleDistanceMeters;
            instance.anchorToBaseSurface = true;
            const std::vector<WorkoutGameProjectedMeshTriangle> triangles =
                    WorkoutGameMeshProjector::project(instance, projection);
            QVERIFY2(!triangles.empty(),
                     qPrintable(QStringLiteral(
                         "terrain %1 projected no visible triangles")
                         .arg(int(piece.terrain))));
            double top = std::numeric_limits<double>::max();
            double bottom = std::numeric_limits<double>::lowest();
            for (const WorkoutGameProjectedMeshTriangle &triangle : triangles) {
                for (const WorkoutGameProjectedMeshVertex &vertex :
                        triangle.vertices) {
                    top = std::min(top, vertex.y);
                    bottom = std::max(bottom, vertex.y);
                }
            }
            QVERIFY2(bottom - top >= 4.0,
                     qPrintable(QStringLiteral(
                         "terrain %1 projected only %2 px high")
                         .arg(int(piece.terrain)).arg(bottom - top)));
            QVERIFY2(top < 720.0 && bottom > 0.0,
                     qPrintable(QStringLiteral(
                         "terrain %1 projected outside viewport: %2..%3")
                         .arg(int(piece.terrain)).arg(top).arg(bottom)));
        }
    }

    void completedJumpRendersAVisibleAirbornePose()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        const int section = 6;
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [section](const WorkoutGameRoadPiece &candidate) {
                    return candidate.sourceSectionIndex
                                == std::size_t(section)
                            && candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameRoadTimelineSection &timeline =
                road.timeline[std::size_t(section)];
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        WorkoutGameSimulationSnapshot simulation;
        simulation.ready = true;
        simulation.activeSection = section;
        simulation.featureOutcome = WorkoutGameFeatureOutcome::Completed;
        simulation.route = WorkoutGameRoute::MainLine;
        simulation.challengeReadiness = 1.0;
        simulation.speedKph = 20.0;
        const double takeoff = piece->challenge.obstacleDistanceMeters - 0.9;
        simulation.sectionProgress = std::clamp(
                (takeoff + 0.1 - timeline.startDistanceMeters)
                    / (timeline.endDistanceMeters
                        - timeline.startDistanceMeters),
                0.0, 1.0);
        const WorkoutGameFeatureRuntimeSnapshot layout =
                runtime.update(simulation);
        const double apexDistance = layout.actionStartDistanceMeters
                + 0.30 * (layout.actionEndDistanceMeters
                          - layout.actionStartDistanceMeters);
        simulation.sectionProgress = std::clamp(
                (apexDistance - timeline.startDistanceMeters)
                    / (timeline.endDistanceMeters
                        - timeline.startDistanceMeters),
                0.0, 1.0);
        simulation.workoutTimeMs = course.sections[section].startMs
                + std::int64_t(std::llround(
                    course.sections[section].durationMs
                        * simulation.sectionProgress));

        WorkoutGameVisualSnapshot airborne;
        airborne.simulation = simulation;
        airborne.feature = runtime.update(simulation);
        airborne.world.ready = true;
        airborne.world.generation = 1;
        airborne.world.terrain = WorkoutGameTerrainKind::Tabletop;
        airborne.world.rider.distanceMeters = apexDistance;
        airborne.world.speedMetersPerSecond = 20.0 / 3.6;
        QVERIFY(airborne.feature.verticalOffsetMeters > 1.0);
        WorkoutGameVisualSnapshot grounded = airborne;
        grounded.feature.verticalOffsetMeters = 0.0;
        grounded.feature.pitchDegrees = 0.0;

        WorkoutGameSceneGraphWindow groundWindow;
        groundWindow.resize(1280, 720);
        groundWindow.setCourse(course, 200.0);
        groundWindow.setFrame(grounded, 230.0, 230.0, 88, 150, 7);
        groundWindow.show();
        QTRY_VERIFY_WITH_TIMEOUT(groundWindow.isExposed(), 3000);
        QTest::qWait(400);
        const QImage groundImage = groundWindow.grabWindow();
        groundWindow.hide();

        WorkoutGameSceneGraphWindow airWindow;
        airWindow.resize(1280, 720);
        airWindow.setCourse(course, 200.0);
        airWindow.setFrame(airborne, 230.0, 230.0, 88, 150, 7);
        airWindow.show();
        QTRY_VERIFY_WITH_TIMEOUT(airWindow.isExposed(), 3000);
        QTest::qWait(400);
        const QImage airImage = airWindow.grabWindow();
        QVERIFY(!groundImage.isNull());
        QVERIFY(!airImage.isNull());
        QVERIFY(changedPixels(groundImage, airImage, 240) > 450);
        const QByteArray requestedAuditDirectory =
                qgetenv("GC_WORKOUT_GAME_JUMP_AUDIT_DIR");
        if (!requestedAuditDirectory.isEmpty()) {
            const QString auditDirectory =
                    QString::fromLocal8Bit(requestedAuditDirectory);
            QVERIFY(QDir().mkpath(auditDirectory));
            QVERIFY(groundImage.save(QDir(auditDirectory).filePath(
                    QStringLiteral("jump-grounded.png"))));
            QVERIFY(airImage.save(QDir(auditDirectory).filePath(
                    QStringLiteral("jump-apex.png"))));
        }
        QVERIFY(airImage.save(QDir(QDir::tempPath()).filePath(
                QStringLiteral("workout-game-jump-apex.png"))));
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
